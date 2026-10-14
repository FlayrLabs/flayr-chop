// A small coverage-guided fuzzer for toolchains without libFuzzer (Apple clang ships none). It drives
// the same LLVMFuzzerTestOneInput / LLVMFuzzerInitialize harnesses libFuzzer does, takes a subset of
// libFuzzer's flags, and is meant to run under ASan + UBSan:
//
//   fuzz_x [flags] corpus_dir [more_dirs...]   fuzz; new inputs go into the first directory
//   fuzz_x [flags] file [more_files...]        run each file once (reproduce a crash, replay fixtures)
//
//   -runs=N  -max_total_time=S  -seed=N  -max_len=N  -timeout=S  -rss_limit_mb=N  -dict=file
//   -artifact_prefix=path  -print_every=S  -only_ascii=0|1 (ignored)
//
// Coverage comes from -fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp on the code under
// test (never on this file). An input is kept when it reaches a new (counter, hit-count bucket) pair.
// Comparisons the target makes (integers and memcmp/strcmp-style calls) are recorded and offered back
// to the mutator, so magic numbers and chunk ids are found without a dictionary.
//
// A crash (ASan/UBSan report, signal, abort, uncaught exception), a timeout or going over the RSS limit
// writes the input to <artifact_prefix>{crash,timeout,oom}-<hash> and stops, like libFuzzer. The
// per-allocation limit is ASan's max_allocation_size_mb (set through ASAN_OPTIONS; default below).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size);
extern "C" __attribute__ ((weak)) int LLVMFuzzerInitialize (int* argc, char*** argv);
extern "C" void __sanitizer_set_death_callback (void (*callback) (void));

// Defaults for every fuzz target: report allocations over 2 GB as bugs instead of returning null,
// no leak checking (JUCE singletons), and abort on the first UBSan report.
extern "C" const char* __asan_default_options()
{
    return "max_allocation_size_mb=2048:allocator_may_return_null=0:detect_leaks=0:handle_abort=1:"
           "handle_segv=1:handle_sigbus=1:handle_sigfpe=1:handle_sigill=1:detect_stack_use_after_return=0";
}
extern "C" const char* __ubsan_default_options() { return "halt_on_error=1:print_stacktrace=1"; }

namespace
{
//==============================================================================
// Coverage
struct Region
{
    uint8_t* begin;
    uint8_t* end;
};
std::vector<Region>& regions()
{
    static std::vector<Region> r;
    return r;
}
std::vector<uint8_t> seenBuckets; // per counter, a bit per hit-count bucket ever seen
size_t totalCounters = 0;

inline uint8_t bucketBit (uint8_t hits)
{
    if (hits >= 128) return 0x80;
    if (hits >= 32) return 0x40;
    if (hits >= 16) return 0x20;
    if (hits >= 8) return 0x10;
    if (hits >= 4) return 0x08;
    if (hits == 3) return 0x04;
    if (hits == 2) return 0x02;
    return 0x01;
}

void clearCounters()
{
    for (auto& r : regions())
        std::memset (r.begin, 0, (size_t) (r.end - r.begin));
}

// New features in the last run; `commit` records them as seen.
size_t collectFeatures (bool commit)
{
    if (seenBuckets.size() != totalCounters)
        seenBuckets.resize (totalCounters, 0);
    size_t fresh = 0, base = 0;
    for (auto& r : regions())
    {
        const size_t n = (size_t) (r.end - r.begin);
        for (size_t i = 0; i < n; ++i)
            if (const uint8_t h = r.begin[i])
            {
                const uint8_t bit = bucketBit (h);
                if ((seenBuckets[base + i] & bit) == 0)
                {
                    ++fresh;
                    if (commit)
                        seenBuckets[base + i] |= bit;
                }
            }
        base += n;
    }
    return fresh;
}

size_t coveredCounters()
{
    size_t n = 0;
    for (auto b : seenBuckets)
        n += b != 0;
    return n;
}
size_t featureCount()
{
    size_t n = 0;
    for (auto b : seenBuckets)
        n += (size_t) __builtin_popcount (b);
    return n;
}

//==============================================================================
// Comparison operands seen during runs ("table of recent compares").
std::atomic<bool> recording { false };
struct IntCmp
{
    uint64_t a, b;
    uint8_t width;
};
constexpr size_t kCmpSlots = 4096;
IntCmp intCmps[kCmpSlots];
struct MemCmp
{
    uint8_t a[32], b[32];
    uint8_t len;
};
constexpr size_t kMemSlots = 1024;
MemCmp memCmps[kMemSlots];

inline size_t slotFor (const void* pc, size_t slots) { return ((uintptr_t) pc >> 2) * 0x9E3779B97F4A7C15ull % slots; }

inline void recordInt (const void* pc, uint64_t a, uint64_t b, uint8_t width)
{
    if (! recording.load (std::memory_order_relaxed) || a == b)
        return;
    auto& e = intCmps[slotFor (pc, kCmpSlots)];
    e.a = a;
    e.b = b;
    e.width = width;
}

inline void recordMem (const void* pc, const void* a, const void* b, size_t n)
{
    if (! recording.load (std::memory_order_relaxed) || n < 2 || a == nullptr || b == nullptr)
        return;
    n = std::min<size_t> (n, 32);
    auto& e = memCmps[slotFor (pc, kMemSlots)];
    std::memcpy (e.a, a, n);
    std::memcpy (e.b, b, n);
    e.len = (uint8_t) n;
}

size_t boundedStrlen (const char* s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != 0)
        ++n;
    return n;
}

//==============================================================================
// Inputs and artefacts
using Bytes = std::vector<uint8_t>;

struct Options
{
    long long runs = -1;
    double maxTotalTime = 0;
    unsigned seed = 0;
    size_t maxLen = 64 * 1024;
    double timeout = 20;
    size_t rssLimitMb = 2048;
    double printEvery = 10;
    std::string artifactPrefix = "./";
    std::vector<std::string> dicts;
    std::vector<std::string> paths;
} opt;

std::string hexHash (const Bytes& b)
{
    // FNV-1a 64, twice with different offsets: a stable name, not a security property.
    uint64_t h1 = 1469598103934665603ull, h2 = 0x84222325cbf29ce4ull;
    for (auto c : b)
    {
        h1 = (h1 ^ c) * 1099511628211ull;
        h2 = (h2 ^ c) * 0x100000001b3ull + 0x9e37;
    }
    char buf[40];
    std::snprintf (buf, sizeof (buf), "%016llx%016llx", (unsigned long long) h1, (unsigned long long) h2);
    return buf;
}

bool readFile (const std::string& path, Bytes& out)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
        return false;
    out.assign (std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>());
    return true;
}

void writeFile (const std::string& path, const Bytes& b)
{
    if (FILE* f = std::fopen (path.c_str(), "wb"))
    {
        if (! b.empty())
            std::fwrite (b.data(), 1, b.size(), f);
        std::fclose (f);
    }
}

bool isDir (const std::string& p)
{
    struct stat st {};
    return ::stat (p.c_str(), &st) == 0 && S_ISDIR (st.st_mode);
}

std::vector<std::string> listDir (const std::string& dir)
{
    std::vector<std::string> out;
    if (DIR* d = ::opendir (dir.c_str()))
    {
        while (auto* e = ::readdir (d))
        {
            if (e->d_name[0] == '.')
                continue;
            const auto p = dir + "/" + e->d_name;
            if (! isDir (p))
                out.push_back (p);
        }
        ::closedir (d);
    }
    std::sort (out.begin(), out.end());
    return out;
}

// The input being run, for the crash handlers (plain storage so a signal handler can write it).
Bytes current;
std::atomic<bool> running { false };
std::atomic<long long> runStartedMs { 0 };
std::atomic<bool> dumping { false };

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

void dumpCurrent (const char* kind)
{
    if (dumping.exchange (true))
        return;
    const auto path = opt.artifactPrefix + kind + "-" + hexHash (current);
    writeFile (path, current);
    std::fprintf (stderr, "\n==fuzz== %s: input of %zu bytes written to %s\n", kind, current.size(), path.c_str());
    std::fflush (stderr);
}

void onDeath() { dumpCurrent ("crash"); }

void onSignal (int sig)
{
    dumpCurrent ("crash");
    std::signal (sig, SIG_DFL);
    std::raise (sig);
}

void onTerminate()
{
    std::fprintf (stderr, "==fuzz== uncaught exception / std::terminate\n");
    dumpCurrent ("crash");
    std::abort();
}

size_t rssMb()
{
#if defined(__APPLE__)
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info (mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS)
        return (size_t) (info.resident_size >> 20);
    return 0;
#else
    long pages = 0, resident = 0;
    if (FILE* f = std::fopen ("/proc/self/statm", "r"))
    {
        if (std::fscanf (f, "%ld %ld", &pages, &resident) != 2)
            resident = 0;
        std::fclose (f);
    }
    return (size_t) resident * (size_t) sysconf (_SC_PAGESIZE) >> 20;
#endif
}

void watchdog()
{
    for (;;)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        if (opt.rssLimitMb > 0)
            if (const auto rss = rssMb(); rss > opt.rssLimitMb)
            {
                std::fprintf (stderr, "\n==fuzz== out of memory: RSS %zu MB > %zu MB\n", rss, opt.rssLimitMb);
                dumpCurrent ("oom");
                std::_Exit (71);
            }
        if (running.load() && opt.timeout > 0 && nowMs() - runStartedMs.load() > (long long) (opt.timeout * 1000.0))
        {
            std::fprintf (stderr, "\n==fuzz== timeout: one input ran for more than %.0f s\n", opt.timeout);
            dumpCurrent ("timeout");
            std::_Exit (70);
        }
    }
}

void runOne (const Bytes& in)
{
    current = in;
    clearCounters();
    runStartedMs = nowMs();
    running = true;
    recording.store (true, std::memory_order_relaxed);
    // A copy of exactly `size` bytes, so ASan sees any read past the end.
    std::unique_ptr<uint8_t[]> copy (new uint8_t[in.size() + 1]);
    if (! in.empty())
        std::memcpy (copy.get(), in.data(), in.size());
    LLVMFuzzerTestOneInput (in.empty() ? nullptr : copy.get(), in.size());
    recording.store (false, std::memory_order_relaxed);
    running = false;
}

//==============================================================================
// Mutations
std::mt19937_64 rng;
size_t rnd (size_t n) { return n == 0 ? 0 : (size_t) (rng() % n); }
bool coin (unsigned oneIn) { return rnd (oneIn) == 0; }

std::vector<Bytes> dictionary;

bool parseDictLine (const std::string& line, Bytes& out)
{
    const auto q1 = line.find ('"');
    const auto q2 = line.rfind ('"');
    if (q1 == std::string::npos || q2 <= q1)
        return false;
    out.clear();
    for (size_t i = q1 + 1; i < q2; ++i)
    {
        if (line[i] == '\\' && i + 1 < q2)
        {
            const char c = line[++i];
            if (c == 'x' && i + 2 < q2 + 1)
            {
                out.push_back ((uint8_t) std::strtoul (line.substr (i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            }
            else
                out.push_back ((uint8_t) c);
        }
        else
            out.push_back ((uint8_t) line[i]);
    }
    return ! out.empty();
}

void loadDict (const std::string& path)
{
    std::ifstream in (path);
    std::string line;
    Bytes b;
    while (std::getline (in, line))
        if (! line.empty() && line[0] != '#' && parseDictLine (line, b))
            dictionary.push_back (b);
}

const uint64_t kInteresting[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 100, 127, 128, 255, 256, 511, 512, 1000, 1023, 1024,
                                  4095, 4096, 8000, 22050, 32767, 32768, 44100, 48000, 65535, 65536, 96000, 192000, 768000,
                                  0x7fffff, 0x800000, 0xffffff, 0x1000000, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff,
                                  0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull };

void putInt (Bytes& b, size_t pos, uint64_t v, int width, bool bigEndian)
{
    for (int i = 0; i < width && pos + (size_t) i < b.size(); ++i)
    {
        const int shift = bigEndian ? 8 * (width - 1 - i) : 8 * i;
        b[pos + (size_t) i] = (uint8_t) (v >> shift);
    }
}

uint64_t getInt (const Bytes& b, size_t pos, int width, bool bigEndian)
{
    uint64_t v = 0;
    for (int i = 0; i < width && pos + (size_t) i < b.size(); ++i)
    {
        const int shift = bigEndian ? 8 * (width - 1 - i) : 8 * i;
        v |= (uint64_t) b[pos + (size_t) i] << shift;
    }
    return v;
}

// Offsets of size fields in chunked containers: RIFF/RF64/W64-style little-endian 4cc + u32,
// AIFF/AIFC big-endian 4cc + u32, FLAC metadata block headers (24-bit big-endian length), Ogg page
// segment tables and ID3 headers are close enough for "a length field lives here".
struct SizeField
{
    size_t pos;
    int width;
    bool bigEndian;
    size_t chunkStart; // where the chunk body starts
    size_t chunkLen;   // what the field says
};

bool isFourCC (const Bytes& b, size_t p)
{
    if (p + 4 > b.size())
        return false;
    for (size_t i = 0; i < 4; ++i)
        if (b[p + i] < 0x20 || b[p + i] > 0x7e)
            return false;
    return true;
}

std::vector<SizeField> findSizeFields (const Bytes& b)
{
    std::vector<SizeField> out;
    auto walk = [&] (size_t start, bool be) {
        size_t p = start;
        for (int guard = 0; guard < 256 && p + 8 <= b.size() && isFourCC (b, p); ++guard)
        {
            const auto len = (size_t) getInt (b, p + 4, 4, be);
            out.push_back ({ p + 4, 4, be, p + 8, len });
            if (len > b.size())
                break;
            p += 8 + len + (len & 1);
        }
    };
    if (b.size() >= 12 && (std::memcmp (b.data(), "RIFF", 4) == 0 || std::memcmp (b.data(), "RF64", 4) == 0))
    {
        out.push_back ({ 4, 4, false, 8, (size_t) getInt (b, 4, 4, false) });
        walk (12, false);
    }
    if (b.size() >= 12 && std::memcmp (b.data(), "FORM", 4) == 0)
    {
        out.push_back ({ 4, 4, true, 8, (size_t) getInt (b, 4, 4, true) });
        walk (12, true);
    }
    for (size_t off : { (size_t) 0, (size_t) 1 }) // the state and stem-cache harnesses put a byte in front
        if (b.size() >= off + 8 && std::memcmp (b.data() + off, "fLaC", 4) == 0)
        {
            size_t p = off + 4;
            for (int guard = 0; guard < 64 && p + 4 <= b.size(); ++guard)
            {
                const auto len = (size_t) getInt (b, p + 1, 3, true);
                out.push_back ({ p + 1, 3, true, p + 4, len });
                const bool last = (b[p] & 0x80) != 0;
                p += 4 + len;
                if (last)
                    break;
            }
        }
    // Anything else that looks like a 4cc followed by a plausible length (embedded chunks, the
    // harnesses' own containers): a few random probes.
    for (int k = 0; k < 4 && b.size() > 8; ++k)
    {
        const size_t p = rnd (b.size() - 8);
        if (isFourCC (b, p))
            for (bool be : { false, true })
                if (const auto len = (size_t) getInt (b, p + 4, 4, be); len <= b.size())
                    out.push_back ({ p + 4, 4, be, p + 8, len });
    }
    return out;
}

void mutateSizeField (Bytes& b)
{
    const auto fields = findSizeFields (b);
    if (fields.empty())
        return;
    const auto& f = fields[rnd (fields.size())];
    const uint64_t mask = f.width >= 8 ? ~0ull : ((1ull << (8 * f.width)) - 1);
    uint64_t v = f.chunkLen;
    switch (rnd (8))
    {
        case 0: v = 0; break;
        case 1: v = f.chunkLen + 1; break;
        case 2: v = f.chunkLen - 1; break;
        case 3: v = mask; break;
        case 4: v = mask >> 1; break;
        case 5: v = b.size() - std::min (b.size(), f.chunkStart); break;
        case 6: v = kInteresting[rnd (sizeof (kInteresting) / sizeof (kInteresting[0]))]; break;
        default: v = f.chunkLen + (uint64_t) rnd (64) - 32; break;
    }
    putInt (b, f.pos, v & mask, f.width, f.bigEndian);
}

void mutateChunks (Bytes& b, const std::vector<Bytes>& corpus)
{
    auto fields = findSizeFields (b);
    fields.erase (std::remove_if (fields.begin(), fields.end(),
                                  [&] (const SizeField& f) { return f.chunkStart < 8 || f.chunkStart + f.chunkLen > b.size(); }),
                  fields.end());
    if (fields.empty())
        return;
    const auto& f = fields[rnd (fields.size())];
    const size_t start = f.chunkStart - 8, len = 8 + f.chunkLen + ((f.chunkLen & 1) && f.chunkStart + f.chunkLen < b.size() ? 1 : 0);
    if (start + len > b.size())
        return;
    switch (rnd (4))
    {
        case 0: // duplicate the chunk
            b.insert (b.begin() + (long) (start + len), b.begin() + (long) start, b.begin() + (long) (start + len));
            break;
        case 1: // delete it
            b.erase (b.begin() + (long) start, b.begin() + (long) (start + len));
            break;
        case 2: // move it to the end
        {
            Bytes chunk (b.begin() + (long) start, b.begin() + (long) (start + len));
            b.erase (b.begin() + (long) start, b.begin() + (long) (start + len));
            b.insert (b.end(), chunk.begin(), chunk.end());
            break;
        }
        default: // splice in a chunk from another input
        {
            const auto& other = corpus[rnd (corpus.size())];
            const auto of = findSizeFields (other);
            if (of.empty())
                break;
            const auto& g = of[rnd (of.size())];
            if (g.chunkStart < 8 || g.chunkStart + g.chunkLen > other.size())
                break;
            b.insert (b.begin() + (long) start, other.begin() + (long) (g.chunkStart - 8), other.begin() + (long) (g.chunkStart + g.chunkLen));
            break;
        }
    }
    // Sometimes fix the outer container size so the change is not refused at the first check.
    if (coin (2) && b.size() >= 8)
    {
        if (std::memcmp (b.data(), "RIFF", 4) == 0)
            putInt (b, 4, b.size() - 8, 4, false);
        else if (std::memcmp (b.data(), "FORM", 4) == 0)
            putInt (b, 4, b.size() - 8, 4, true);
    }
}

void mutateFromCmps (Bytes& b)
{
    if (b.empty())
        return;
    if (coin (3))
    {
        const auto& e = memCmps[rnd (kMemSlots)];
        if (e.len == 0)
            return;
        const uint8_t* from = coin (2) ? e.a : e.b;
        const uint8_t* to = from == e.a ? e.b : e.a;
        // Replace an occurrence of one operand with the other, else write it somewhere.
        for (size_t i = 0; i + e.len <= b.size(); ++i)
            if (std::memcmp (b.data() + i, from, e.len) == 0)
            {
                std::memcpy (b.data() + i, to, e.len);
                return;
            }
        const size_t pos = rnd (b.size());
        if (coin (2))
            b.insert (b.begin() + (long) pos, to, to + e.len);
        else
            for (size_t i = 0; i < e.len && pos + i < b.size(); ++i)
                b[pos + i] = to[i];
        return;
    }
    const auto& e = intCmps[rnd (kCmpSlots)];
    if (e.width == 0)
        return;
    const bool flip = coin (2);
    const uint64_t from = flip ? e.a : e.b, to = flip ? e.b : e.a;
    for (const bool be : { false, true })
        for (size_t i = 0; i + e.width <= b.size(); ++i)
            if (getInt (b, i, e.width, be) == from)
            {
                putInt (b, i, to + (coin (4) ? (uint64_t) rnd (3) - 1 : 0), e.width, be);
                return;
            }
    putInt (b, rnd (b.size()), to, e.width, coin (2));
}

void mutate (Bytes& b, const std::vector<Bytes>& corpus)
{
    const int count = 1 + (int) rnd (coin (2) ? 2 : 8);
    for (int k = 0; k < count; ++k)
    {
        switch (rnd (16))
        {
            case 0: // flip a bit
                if (! b.empty()) b[rnd (b.size())] ^= (uint8_t) (1u << rnd (8));
                break;
            case 1: // random byte
                if (! b.empty()) b[rnd (b.size())] = (uint8_t) rng();
                break;
            case 2: // insert bytes
            {
                const size_t n = 1 + rnd (coin (4) ? 128 : 4);
                const uint8_t v = (uint8_t) rng();
                const bool same = coin (2);
                Bytes ins (n);
                for (auto& c : ins)
                    c = same ? v : (uint8_t) rng();
                b.insert (b.begin() + (long) rnd (b.size() + 1), ins.begin(), ins.end());
                break;
            }
            case 3: // delete bytes
                if (! b.empty())
                {
                    const size_t pos = rnd (b.size());
                    const size_t n = std::min (b.size() - pos, 1 + rnd (coin (4) ? 256 : 8));
                    b.erase (b.begin() + (long) pos, b.begin() + (long) (pos + n));
                }
                break;
            case 4: // copy a block within the input
                if (b.size() > 2)
                {
                    const size_t from = rnd (b.size()), to = rnd (b.size());
                    const size_t n = std::min ({ b.size() - from, b.size() - to, 1 + rnd (64) });
                    std::memmove (b.data() + to, b.data() + from, n);
                }
                break;
            case 5:
            case 6: // integer boundary values, either endianness, 1/2/4/8 bytes
                if (! b.empty())
                {
                    const int widths[] = { 1, 2, 4, 8 };
                    const int w = widths[rnd (4)];
                    uint64_t v = kInteresting[rnd (sizeof (kInteresting) / sizeof (kInteresting[0]))];
                    if (coin (4))
                        v = ~v + 1; // two's-complement negative
                    putInt (b, rnd (b.size()), v, w, coin (2));
                }
                break;
            case 7: // add or subtract a little from an integer
                if (b.size() >= 4)
                {
                    const int w = coin (2) ? 2 : 4;
                    const bool be = coin (2);
                    const size_t pos = rnd (b.size() - (size_t) w + 1);
                    putInt (b, pos, getInt (b, pos, w, be) + (uint64_t) rnd (71) - 35, w, be);
                }
                break;
            case 8:
            case 9: // container size fields
                mutateSizeField (b);
                break;
            case 10: // whole chunks
                mutateChunks (b, corpus);
                break;
            case 11:
            case 12: // values the target compared against
                mutateFromCmps (b);
                break;
            case 13: // dictionary
                if (! dictionary.empty())
                {
                    const auto& d = dictionary[rnd (dictionary.size())];
                    const size_t pos = rnd (b.size() + 1);
                    if (coin (2) || pos + d.size() > b.size())
                        b.insert (b.begin() + (long) pos, d.begin(), d.end());
                    else
                        std::copy (d.begin(), d.end(), b.begin() + (long) pos);
                }
                break;
            case 14: // crossover
            {
                const auto& o = corpus[rnd (corpus.size())];
                if (o.empty())
                    break;
                const size_t from = rnd (o.size()), n = std::min (o.size() - from, 1 + rnd (512));
                const size_t pos = rnd (b.size() + 1);
                if (coin (2))
                    b.insert (b.begin() + (long) pos, o.begin() + (long) from, o.begin() + (long) (from + n));
                else
                    for (size_t i = 0; i < n && pos + i < b.size(); ++i)
                        b[pos + i] = o[from + i];
                break;
            }
            default: // truncate or extend
                if (coin (2) && ! b.empty())
                    b.resize (rnd (b.size()) + 1);
                else
                    b.resize (b.size() + 1 + rnd (64), (uint8_t) (coin (2) ? 0 : rng()));
                break;
        }
    }
    if (b.size() > opt.maxLen)
        b.resize (opt.maxLen);
}

bool parseFlag (const char* a)
{
    auto val = [a] (const char* name) -> const char* {
        const size_t n = std::strlen (name);
        return std::strncmp (a, name, n) == 0 && a[n] == '=' ? a + n + 1 : nullptr;
    };
    if (auto v = val ("-runs")) opt.runs = std::atoll (v);
    else if (auto v2 = val ("-max_total_time")) opt.maxTotalTime = std::atof (v2);
    else if (auto v3 = val ("-seed")) opt.seed = (unsigned) std::strtoul (v3, nullptr, 10);
    else if (auto v4 = val ("-max_len")) opt.maxLen = (size_t) std::atoll (v4);
    else if (auto v5 = val ("-timeout")) opt.timeout = std::atof (v5);
    else if (auto v6 = val ("-rss_limit_mb")) opt.rssLimitMb = (size_t) std::atoll (v6);
    else if (auto v7 = val ("-artifact_prefix")) opt.artifactPrefix = v7;
    else if (auto v8 = val ("-dict")) opt.dicts.push_back (v8);
    else if (auto v9 = val ("-print_every")) opt.printEvery = std::atof (v9);
    else if (a[0] == '-') std::fprintf (stderr, "==fuzz== ignoring flag %s\n", a);
    else return false;
    return true;
}
} // namespace

//==============================================================================
// SanitizerCoverage callbacks (the code under test is built with them; this file is not).
extern "C" void __sanitizer_cov_8bit_counters_init (uint8_t* begin, uint8_t* end)
{
    if (begin == end)
        return;
    for (const auto& r : regions())
        if (r.begin == begin)
            return;
    regions().push_back ({ begin, end });
    totalCounters += (size_t) (end - begin);
}
extern "C" void __sanitizer_cov_pcs_init (const uintptr_t*, const uintptr_t*) {}

#define FUZZ_CMP(name, type, width)                                                                                \
    extern "C" void name (type a, type b) { recordInt (__builtin_return_address (0), (uint64_t) a, (uint64_t) b, width); }
FUZZ_CMP (__sanitizer_cov_trace_cmp1, uint8_t, 1)
FUZZ_CMP (__sanitizer_cov_trace_cmp2, uint16_t, 2)
FUZZ_CMP (__sanitizer_cov_trace_cmp4, uint32_t, 4)
FUZZ_CMP (__sanitizer_cov_trace_cmp8, uint64_t, 8)
FUZZ_CMP (__sanitizer_cov_trace_const_cmp1, uint8_t, 1)
FUZZ_CMP (__sanitizer_cov_trace_const_cmp2, uint16_t, 2)
FUZZ_CMP (__sanitizer_cov_trace_const_cmp4, uint32_t, 4)
FUZZ_CMP (__sanitizer_cov_trace_const_cmp8, uint64_t, 8)
#undef FUZZ_CMP
extern "C" void __sanitizer_cov_trace_switch (uint64_t val, uint64_t* cases)
{
    // cases[0] = count, cases[1] = bit width, then the case values.
    if (cases[0] > 0 && recording.load (std::memory_order_relaxed))
        recordInt (__builtin_return_address (0), val, cases[2 + val % cases[0]], (uint8_t) (cases[1] / 8));
}
extern "C" void __sanitizer_cov_trace_div4 (uint32_t) {}
extern "C" void __sanitizer_cov_trace_div8 (uint64_t) {}
extern "C" void __sanitizer_cov_trace_gep (uintptr_t) {}

extern "C" void __sanitizer_weak_hook_memcmp (void* pc, const void* a, const void* b, size_t n, int result)
{
    if (result != 0) recordMem (pc, a, b, n);
}
extern "C" void __sanitizer_weak_hook_strncmp (void* pc, const char* a, const char* b, size_t n, int result)
{
    if (result != 0) recordMem (pc, a, b, std::min (boundedStrlen (a, n), boundedStrlen (b, n)) + 1);
}
extern "C" void __sanitizer_weak_hook_strcmp (void* pc, const char* a, const char* b, int result)
{
    if (result != 0) recordMem (pc, a, b, std::min (boundedStrlen (a, 32), boundedStrlen (b, 32)) + 1);
}
extern "C" void __sanitizer_weak_hook_strncasecmp (void* pc, const char* a, const char* b, size_t n, int result)
{
    if (result != 0) recordMem (pc, a, b, std::min (boundedStrlen (a, n), boundedStrlen (b, n)) + 1);
}
extern "C" void __sanitizer_weak_hook_strcasecmp (void* pc, const char* a, const char* b, int result)
{
    if (result != 0) recordMem (pc, a, b, std::min (boundedStrlen (a, 32), boundedStrlen (b, 32)) + 1);
}

//==============================================================================
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::setvbuf (stderr, nullptr, _IONBF, 0);

    if (LLVMFuzzerInitialize != nullptr)
        LLVMFuzzerInitialize (&argc, &argv);

    for (int i = 1; i < argc; ++i)
        if (! parseFlag (argv[i]))
            opt.paths.push_back (argv[i]);
    for (const auto& d : opt.dicts)
        loadDict (d);
    rng.seed (opt.seed != 0 ? opt.seed : (unsigned) std::random_device {}());

    __sanitizer_set_death_callback (onDeath);
    std::set_terminate (onTerminate);
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT })
        std::signal (sig, onSignal);
    std::thread (watchdog).detach();

    const bool fuzzing = ! opt.paths.empty() && isDir (opt.paths[0]);
    if (! fuzzing)
    {
        // Replay: run each file once.
        for (const auto& p : opt.paths)
        {
            Bytes b;
            if (! readFile (p, b))
            {
                std::fprintf (stderr, "==fuzz== can't read %s\n", p.c_str());
                return 1;
            }
            std::printf ("Running: %s (%zu bytes)\n", p.c_str(), b.size());
            runOne (b);
        }
        std::printf ("Executed %zu inputs\n", opt.paths.size());
        return 0;
    }

    // Load the corpus (every directory given), keeping only inputs that add coverage.
    std::vector<Bytes> corpus;
    const std::string outDir = opt.paths[0];
    size_t loaded = 0;
    for (const auto& dir : opt.paths)
        for (const auto& f : listDir (dir))
        {
            Bytes b;
            if (! readFile (f, b))
                continue;
            if (b.size() > opt.maxLen)
                b.resize (opt.maxLen);
            ++loaded;
            runOne (b);
            if (collectFeatures (true) > 0 || corpus.empty())
                corpus.push_back (std::move (b));
        }
    if (corpus.empty())
    {
        runOne ({});
        collectFeatures (true);
        corpus.push_back ({});
    }
    std::printf ("#0 INITED corpus %zu/%zu inputs, cov %zu ft %zu counters %zu\n", corpus.size(), loaded, coveredCounters(),
                 featureCount(), totalCounters);

    std::unordered_set<std::string> saved;
    for (const auto& f : listDir (outDir))
        saved.insert (f.substr (f.find_last_of ('/') + 1));

    const auto t0 = nowMs();
    auto lastPrint = t0;
    long long execs = 0;
    for (;;)
    {
        if (opt.runs >= 0 && execs >= opt.runs)
            break;
        const auto now = nowMs();
        if (opt.maxTotalTime > 0 && (double) (now - t0) / 1000.0 >= opt.maxTotalTime)
            break;
        // Recently added inputs are picked more often.
        const size_t n = corpus.size();
        const size_t pick = coin (2) ? n - 1 - rnd (std::min<size_t> (n, 16)) : rnd (n);
        Bytes b = corpus[pick];
        mutate (b, corpus);
        runOne (b);
        ++execs;
        if (collectFeatures (true) > 0)
        {
            const auto name = hexHash (b);
            if (saved.insert (name).second)
                writeFile (outDir + "/" + name, b);
            corpus.push_back (std::move (b));
            std::printf ("#%lld NEW cov %zu ft %zu corpus %zu len %zu\n", execs, coveredCounters(), featureCount(), corpus.size(),
                         corpus.back().size());
        }
        if ((double) (now - lastPrint) / 1000.0 >= opt.printEvery)
        {
            lastPrint = now;
            const double secs = (double) (now - t0) / 1000.0;
            std::printf ("#%lld pulse cov %zu ft %zu corpus %zu exec/s %.0f rss %zuMB t %.0fs\n", execs, coveredCounters(), featureCount(),
                         corpus.size(), secs > 0 ? (double) execs / secs : 0.0, rssMb(), secs);
        }
    }
    const double secs = (double) (nowMs() - t0) / 1000.0;
    std::printf ("Done %lld runs in %.0f second(s): cov %zu ft %zu corpus %zu\n", execs, secs, coveredCounters(), featureCount(),
                 corpus.size());
    return 0;
}
