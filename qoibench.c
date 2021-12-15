/*

Simple benchmark suite for spng, stbi and qoi

Requires spng.c/.h, stb_image.h and stb_image_write.h

Dominic Szablewski - https://phoboslab.org


-- LICENSE: The MIT License(MIT)

Copyright(c) 2021 Dominic Szablewski, Miles Fogle

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files(the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions :
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include "spng.h"




// -----------------------------------------------------------------------------
// Cross platform high resolution timer
// From https://gist.github.com/ForeverZer0/0a4f80fc02b96e19380ebb7a3debbee5

#include <stdint.h>
#if defined(__linux)
	#define HAVE_POSIX_TIMER
	#include <time.h>
	#ifdef CLOCK_MONOTONIC
		#define CLOCKID CLOCK_MONOTONIC
	#else
		#define CLOCKID CLOCK_REALTIME
	#endif
#elif defined(__APPLE__)
	#define HAVE_MACH_TIMER
	#include <mach/mach_time.h>
#elif defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#endif

static uint64_t ns() {
	static uint64_t is_init = 0;
#if defined(__APPLE__)
		static mach_timebase_info_data_t info;
		if (0 == is_init) {
			mach_timebase_info(&info);
			is_init = 1;
		}
		uint64_t now;
		now = mach_absolute_time();
		now *= info.numer;
		now /= info.denom;
		return now;
#elif defined(__linux)
		static struct timespec linux_rate;
		if (0 == is_init) {
			clock_getres(CLOCKID, &linux_rate);
			is_init = 1;
		}
		uint64_t now;
		struct timespec spec;
		clock_gettime(CLOCKID, &spec);
		now = spec.tv_sec * 1.0e9 + spec.tv_nsec;
		return now;
#elif defined(_WIN32)
		static LARGE_INTEGER win_frequency;
		if (0 == is_init) {
			QueryPerformanceFrequency(&win_frequency);
			is_init = 1;
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return (uint64_t) ((1e9 * now.QuadPart)	/ win_frequency.QuadPart);
#endif
}

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define ERROR_EXIT(...) printf("abort at line " TOSTRING(__LINE__) ": " __VA_ARGS__); printf("\n"); exit(1)


// -----------------------------------------------------------------------------
// stb_image encode callback

void stbi_write_callback(void *context, void *data, int size) {
	int *encoded_size = (int *)context;
	*encoded_size += size;
	// In theory we'd need to do another malloc(), memcpy() and free() here to 
	// be fair to the other decode functions...
}


// -----------------------------------------------------------------------------
// function to load a whole file into memory

void *fload(const char *path, int *out_size) {
	FILE *fh = fopen(path, "rb");
	if (!fh) {
		ERROR_EXIT("Can't open file");
	}

	fseek(fh, 0, SEEK_END);
	int size = ftell(fh);
	fseek(fh, 0, SEEK_SET);

	void *buffer = malloc(size);
	if (!buffer) {
		ERROR_EXIT("Malloc for %d bytes failed", size);
	}

	if (!fread(buffer, size, 1, fh)) {
		ERROR_EXIT("Can't read file %s", path);
	}
	fclose(fh);

	*out_size = size;
	return buffer;
}


// -----------------------------------------------------------------------------
// spng wrapper (because fuck trying to build libpng on windows)

void * spng_decode(void * input, size_t inputSize) {
	spng_ctx * ctx = spng_ctx_new(0);
	spng_set_png_buffer(ctx, input, inputSize);
	spng_set_crc_action(ctx, SPNG_CRC_USE, SPNG_CRC_USE); //ignore CRC for maybe slightly faster decoding?
	size_t outputSize = 0;
	spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &outputSize);
	void * output = malloc(outputSize);
	spng_decode_image(ctx, output, outputSize, SPNG_FMT_RGBA8, 0);
	spng_ctx_free(ctx);
	return output;
}

void * spng_encode(void * input, size_t width, size_t height, size_t * outputSize) {
	spng_ctx * ctx = spng_ctx_new(SPNG_CTX_ENCODER);
	spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1);
	spng_ihdr ihdr = { (unsigned) width, (unsigned) height, 8, 6, 0, 0, 0 };
	spng_set_ihdr(ctx, &ihdr);
	spng_encode_image(ctx, input, width * height * 4, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
	int error = 0;
	void * output = spng_get_png_buffer(ctx, outputSize, &error);
	spng_ctx_free(ctx);
	return output;
}


// -----------------------------------------------------------------------------
// cross-platform directory walking

#include "list.hpp"
#include <algorithm> //std::sort
#include <stdarg.h> //va_list etc.
#include <stdio.h> //vsnprintf
#include <string.h> //strncpy, strlen
#include <stdlib.h> //malloc

static inline int case_insensitive_ascii_compare(const char * a, const char * b) {
    for (int i = 0; a[i] || b[i]; ++i) {
        if (tolower(a[i]) != tolower(b[i])) {
            return tolower(a[i]) - tolower(b[i]);
        }
    }
    return 0;
}

static inline char * dup(const char * src, int len) {
    char * ret = (char *) malloc(len + 1);
    strncpy(ret, src, len);
    ret[len] = '\0';
    return ret;
}

static inline char * dup(const char * src) {
    if (!src) return nullptr;
    return dup(src, strlen(src));
}

//allocates a buffer large enough to fit resulting string, and `sprintf`s to it
__attribute__((format(printf, 2, 3)))
static inline char * dsprintf(char * buf, const char * fmt, ...) {
    size_t len = buf? strlen(buf) : 0;
    va_list args1, args2;
    va_start(args1, fmt);
    va_copy(args2, args1);
    buf = (char *) realloc(buf, len + vsnprintf(nullptr, 0, fmt, args1) + 1);
    vsprintf(buf + len, fmt, args2);
    va_end(args1);
    va_end(args2);
    return buf;
}

struct DirEnt {
	char * name;
	bool isDir;
	List<DirEnt> children; //only for directories
};

static inline bool operator<(DirEnt l, DirEnt r) {
    if (l.isDir && !r.isDir) return true;
    if (!l.isDir && r.isDir) return false;
    return case_insensitive_ascii_compare(l.name, r.name) < 0;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

List<DirEnt> fetch_dir_info_recursive(const char * dirpath) {
    char * wildcard = dsprintf(nullptr, "%s/*", dirpath);
    WIN32_FIND_DATAA findData = {};
    HANDLE handle = FindFirstFileA(wildcard, &findData);
    free(wildcard);

    if (handle == INVALID_HANDLE_VALUE) {
        int err = GetLastError();
        assert(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
        return {};
    }

    List<DirEnt> dir = {};
    do {
        if (strcmp(findData.cFileName, ".") && strcmp(findData.cFileName, "..")) {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                char * subdirpath = dsprintf(nullptr, "%s/%s", dirpath, findData.cFileName);
                dir.add({ dup(findData.cFileName), true, fetch_dir_info_recursive(subdirpath) });
                free(subdirpath);
            } else {
                dir.add({ dup(findData.cFileName), false });
            }
        }
    } while (FindNextFileA(handle, &findData));
    assert(GetLastError() == ERROR_NO_MORE_FILES);
    assert(FindClose(handle));

    //sort entries alphabetically
    std::sort(dir.begin(), dir.end());
    return dir;
}

#else
#include <dirent.h>

List<DirEnt> fetch_dir_info_recursive(const char * dirpath) {
    //open directory
    DIR * dp = opendir(dirpath);
    assert(dp);

    List<DirEnt> dir = {};
    dirent * ep = readdir(dp);
    while (ep) {
        if (strcmp(ep->d_name, ".") && strcmp(ep->d_name, "..") && strcmp(ep->d_name, ".DS_Store")) {
            if (ep->d_type == DT_DIR) {
                char * subdirpath = dsprintf(nullptr, "%s/%s", dirpath, ep->d_name);
                dir.add({ dup(ep->d_name), true, fetch_dir_info_recursive(subdirpath) });
                free(subdirpath);
            } else if (ep->d_type == DT_REG) {
                dir.add({ dup(ep->d_name), false });
            }
        }
        ep = readdir(dp);
    }
    closedir(dp);

    //sort entries alphabetically
    std::sort(dir.begin(), dir.end());
    return dir;
}

#endif

// -----------------------------------------------------------------------------
// benchmark runner

typedef struct {
	uint64_t size;
	uint64_t encode_time;
	uint64_t decode_time;
} benchmark_lib_result_t;

typedef struct {
	uint64_t sizeOnDisk;
	uint64_t px;
	int w;
	int h;
	benchmark_lib_result_t spng;
	benchmark_lib_result_t stbi;
	benchmark_lib_result_t qoi;
} benchmark_result_t;


// Run __VA_ARGS__ a number of times and meassure the time taken. The first
// run is ignored.
#define BENCHMARK_FN(RUNS, AVG_TIME, ...) \
	do { \
		uint64_t time = 0; \
		for (int i = 0; i <= RUNS; i++) { \
			uint64_t time_start = ns(); \
			__VA_ARGS__ \
			uint64_t time_end = ns(); \
			if (i > 0) { \
				time += time_end - time_start; \
			} \
		} \
		AVG_TIME = time / RUNS; \
	} while (0)


benchmark_result_t benchmark_image(const char *path, int runs) {
	int encoded_png_size;
	int encoded_qoi_size;
	int w;
	int h;

	// Load the encoded PNG, encoded QOI and raw pixels into memory
	void *pixels = (void *)stbi_load(path, &w, &h, NULL, 4);
	void *encoded_png = fload(path, &encoded_png_size);
	qoi_desc qoiDesc = {
		.width = (unsigned) w,
		.height = (unsigned) h,
		.channels = 4,
		.colorspace = QOI_SRGB
	};
	void *encoded_qoi = qoi_encode(pixels, &qoiDesc, &encoded_qoi_size);

	if (!pixels || !encoded_qoi || !encoded_png) {
		ERROR_EXIT("Error decoding %s\n", path);
	}

	benchmark_result_t res = {0};
	res.sizeOnDisk = encoded_png_size;
	res.px = w * h;
	res.w = w;
	res.h = h;


	// Decoding

	BENCHMARK_FN(runs, res.spng.decode_time, {
		void *dec_p = spng_decode(encoded_png, encoded_png_size);
		free(dec_p);
	});

	BENCHMARK_FN(runs, res.stbi.decode_time, {
		int dec_w, dec_h, dec_channels;
		void *dec_p = stbi_load_from_memory((unsigned char *) encoded_png, encoded_png_size, &dec_w, &dec_h, &dec_channels, 4);
		free(dec_p);
	});

	BENCHMARK_FN(runs, res.qoi.decode_time, {
		qoi_desc desc;
		void *dec_p = qoi_decode(encoded_qoi, encoded_qoi_size, &desc, 4);
		free(dec_p);
	});


	// Encoding

	BENCHMARK_FN(runs, res.spng.encode_time, {
		size_t enc_size = 0;
		void *enc_p = spng_encode(pixels, w, h, &enc_size);
		res.spng.size = enc_size;
		free(enc_p);
	});

	BENCHMARK_FN(runs, res.stbi.encode_time, {
		int enc_size = 0;
		stbi_write_png_to_func(stbi_write_callback, &enc_size, w, h, 4, pixels, 0);
		res.stbi.size = enc_size;
	});

	BENCHMARK_FN(runs, res.qoi.encode_time, {
		int enc_size;
		qoi_desc qoiDesc = {
			.width = (unsigned) w,
			.height = (unsigned) h,
			.channels = 4,
			.colorspace = QOI_SRGB
		};
		void *enc_p = qoi_encode(pixels, &qoiDesc, &enc_size);
		res.qoi.size = enc_size;
		free(enc_p);
	});

	free(pixels);
	free(encoded_png);
	free(encoded_qoi);

	return res;
}

void benchmark_print_lib(const char * name, benchmark_lib_result_t res, double px, double sizeOnDisk) {
	printf("%-5s    %8.1f    %8.1f      %8.2f      %8.2f  %8llu %8.3f  %8.2f\n", name,
		   res.decode_time / 1000000.0,
		   res.encode_time / 1000000.0,
		   res.decode_time > 0 ? px / (res.decode_time / 1000.0) : 0,
		   res.encode_time > 0 ? px / (res.encode_time / 1000.0) : 0,
		   res.size / 1024, res.size / (px * 4), res.size / sizeOnDisk);
}

void benchmark_print_result(const char *head, benchmark_result_t res) {
	printf("## %s size: %dx%d (%llu kb)\n", head, res.w, res.h, res.sizeOnDisk / 1024);
	printf("        decode ms   encode ms   decode mpps   encode mpps   size kb   vs raw   vs best\n");
	benchmark_print_lib("spng", res.spng, res.px, res.sizeOnDisk);
	benchmark_print_lib("stbi", res.stbi, res.px, res.sizeOnDisk);
	benchmark_print_lib("qoi", res.qoi, res.px, res.sizeOnDisk);
	printf("\n");
	fflush(stdout);
}

int main(int argc, char **argv) {
	if (argc < 3) {
		printf("Usage: qoibench <iterations> <directory>\n");
		printf("Example: qoibench 10 images/textures/\n");
		exit(1);
	}

	float total_percentage = 0;
	int total_size = 0;

	benchmark_result_t totals = {0};

	int runs = atoi(argv[1]);
	if (runs <= 0) runs = 1;
	List<DirEnt> files = fetch_dir_info_recursive(argv[2]);

	printf("## Benchmarking %s/*.png -- %d runs\n\n", argv[2], runs); fflush(stdout);
	int count = 0;
	for (DirEnt & file : files) {
		if (file.isDir || strlen(file.name) < 4 || strcmp(file.name + strlen(file.name) - 4, ".png")) {
			continue;
		}
		count += 1;

		char *file_path = (char *) malloc(strlen(file.name) + strlen(argv[2]) + 8);
		sprintf(file_path, "%s/%s", argv[2], file.name);
		
		benchmark_result_t res = benchmark_image(file_path, runs);
		benchmark_print_result(file_path, res);

		free(file_path);

		totals.sizeOnDisk += res.sizeOnDisk;
		totals.px += res.px;
		totals.spng.encode_time += res.spng.encode_time;
		totals.spng.decode_time += res.spng.decode_time;
		totals.spng.size += res.spng.size;
		totals.stbi.encode_time += res.stbi.encode_time;
		totals.stbi.decode_time += res.stbi.decode_time;
		totals.stbi.size += res.stbi.size;
		totals.qoi.encode_time += res.qoi.encode_time;
		totals.qoi.decode_time += res.qoi.decode_time;
		totals.qoi.size += res.qoi.size;
	}

	if (!count) { ERROR_EXIT("No PNG files in this directory"); }
	totals.sizeOnDisk /= count;
	totals.px /= count;
	totals.spng.encode_time /= count;
	totals.spng.decode_time /= count;
	totals.spng.size /= count;
	totals.stbi.encode_time /= count;
	totals.stbi.decode_time /= count;
	totals.stbi.size /= count;
	totals.qoi.encode_time /= count;
	totals.qoi.decode_time /= count;
	totals.qoi.size /= count;

	benchmark_print_result("Totals (AVG)", totals);

	return 0;
}
