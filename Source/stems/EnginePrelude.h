#pragma once
// Every standard and system header the stem engine (Eigen + demucs.cpp) can
// pull in, included up front by the instruction-set specific engine
// translation units (see EngineAvx2.cpp) BEFORE their target region opens.
//
// Why: inside that region the compiler generates AVX2/FMA code. Templates
// and inline functions from the C++ standard library are shared between
// translation units by the linker (one copy wins), so they must be parsed
// outside the region; otherwise an AVX2 copy of, say, std::vector<float>
// could be the one the linker keeps and the generic path would crash on a
// CPU without AVX2. Headers are include-guarded, so a later #include of any
// of these from inside the region is a no-op.
//
// If the engine starts using another standard or system header, add it
// here. To verify a build: disassemble an x86-64 binary (objdump -d -C) and
// check that every function containing VEX / ymm instructions is either in
// the flayr_avx2_* / chop::stems::detail::avx2 namespaces or a file-local
// (nm: lowercase t) helper of the AVX2 translation unit. Nothing automates
// this check yet.

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <clocale>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <deque>
#include <exception>
#include <filesystem>
#include <forward_list>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <iosfwd>
#include <iostream>
#include <istream>
#include <iterator>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <ostream>
#include <queue>
#include <random>
#include <ratio>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <valarray>
#include <variant>
#include <vector>

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#include <malloc.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#include <immintrin.h>
#include <mmintrin.h>
#include <nmmintrin.h>
#include <pmmintrin.h>
#include <smmintrin.h>
#include <tmmintrin.h>
#include <xmmintrin.h>
#if !defined(_MSC_VER) || defined(__clang__)
#include <x86intrin.h>
#endif
#endif
#if defined(__APPLE__)
#include <Availability.h>
#include <TargetConditionals.h>
#if defined(EIGEN_USE_BLAS)
#include <vecLib/vDSP.h>
#include <vecLib/vForce.h>
#endif
#endif
