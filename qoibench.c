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

#if 0 //TODO: make it work with libpng on mac
// -----------------------------------------------------------------------------
// libpng encode/decode wrappers
// Seriously, who thought this was a good abstraction for an API to read/write
// images?

typedef struct {
	int size;
	int capacity;
	unsigned char *data;
} libpng_write_t;

void libpng_encode_callback(png_structp png_ptr, png_bytep data, png_size_t length) {
	libpng_write_t *write_data = (libpng_write_t*)png_get_io_ptr(png_ptr);
	if (write_data->size + length >= write_data->capacity) {
		ERROR("PNG write");
	}
	memcpy(write_data->data + write_data->size, data, length);
	write_data->size += length;
}

void *libpng_encode(void *pixels, int w, int h, int channels, int *out_len) {
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png) {
		ERROR("png_create_write_struct");
	}

	png_infop info = png_create_info_struct(png);
	if (!info) {
		ERROR("png_create_info_struct");
	}

	if (setjmp(png_jmpbuf(png))) {
		ERROR("png_jmpbuf");
	}

	// Output is 8bit depth, RGBA format.
	png_set_IHDR(
		png,
		info,
		w, h,
		8,
		channels == 3 ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT
	);

	png_bytep row_pointers[h];
	for(int y = 0; y < h; y++){
		row_pointers[y] = ((unsigned char *)pixels + y * w * channels);
	}

	libpng_write_t write_data = {
		.size = 0,
		.capacity = w * h * channels,
		.data = malloc(w * h * channels)
	};

	png_set_rows(png, info, row_pointers);
	png_set_write_fn(png, &write_data, libpng_encode_callback, NULL);
	png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);

	png_destroy_write_struct(&png, &info);

	*out_len = write_data.size;
	return write_data.data;
}


typedef struct {
	int pos;
	int size;
	unsigned char *data;
} libpng_read_t;

void png_decode_callback(png_structp png, png_bytep data, png_size_t length) {
	libpng_read_t *read_data = (libpng_read_t*)png_get_io_ptr(png);
	if (read_data->pos + length > read_data->size) {
		ERROR("PNG read %d bytes at pos %d (size: %d)", length, read_data->pos, read_data->size);
	}
	memcpy(data, read_data->data + read_data->pos, length);
	read_data->pos += length;
}

void png_warning_callback(png_structp png_ptr, png_const_charp warning_msg) {
	// Ingore warnings about sRGB profiles and such.
}

void *libpng_decode(void *data, int size, int *out_w, int *out_h) {
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, png_warning_callback);
	if (!png) {
		ERROR("png_create_read_struct");
	}

	png_infop info = png_create_info_struct(png);
	if (!info) {
		ERROR("png_create_info_struct");
	}

	libpng_read_t read_data = {
		.pos = 0,
		.size = size,
		.data = data
	};

	png_set_read_fn(png, &read_data, png_decode_callback);
	png_set_sig_bytes(png, 0);
	png_read_info(png, info);

	png_uint_32 w, h;
	int bitDepth, colorType, interlaceType;
	png_get_IHDR(png, info, &w, &h, &bitDepth, &colorType, &interlaceType, NULL, NULL);

	// 16 bit -> 8 bit
	png_set_strip_16(png);

	// 1, 2, 4 bit -> 8 bit
	if (bitDepth < 8) {
		png_set_packing(png);
	}

	if (colorType & PNG_COLOR_MASK_PALETTE) {
		png_set_expand(png);
	}

	if (!(colorType & PNG_COLOR_MASK_COLOR)) {
		png_set_gray_to_rgb(png);
	}

	// set paletted or RGB images with transparency to full alpha so we get RGBA
	if (png_get_valid(png, info, PNG_INFO_tRNS)) {
		png_set_tRNS_to_alpha(png);
	}

	// make sure every pixel has an alpha value
	if (!(colorType & PNG_COLOR_MASK_ALPHA)) {
		png_set_filler(png, 255, PNG_FILLER_AFTER);
	}

	png_read_update_info(png, info);

	unsigned char* out = malloc(w * h * 4);
	*out_w = w;
	*out_h = h;

	// png_uint_32 rowBytes = png_get_rowbytes(png, info);
	png_bytep row_pointers[h];
	for (png_uint_32 row = 0; row < h; row++ ) {
		row_pointers[row] = (png_bytep)(out + (row * w * 4));
	}

	png_read_image(png, row_pointers);
	png_read_end(png, info);
	png_destroy_read_struct( &png, &info, NULL);

	return out;
}
#endif


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

void * spng_encode(void * input, size_t width, size_t height, int channels, size_t * outputSize) {
	spng_ctx * ctx = spng_ctx_new(SPNG_CTX_ENCODER);
	spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1);
	int colorType = channels == 4? 6 : 2;
	spng_ihdr ihdr = { (unsigned) width, (unsigned) height, 8, (unsigned char) colorType, 0, 0, 0 };
	spng_set_ihdr(ctx, &ihdr);
	spng_encode_image(ctx, input, width * height * channels, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
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

int opt_runs = 1;
int opt_nopng = 0;
int opt_nowarmup = 0;
int opt_noverify = 0;
int opt_nodecode = 0;
int opt_noencode = 0;
int opt_norecurse = 0;
int opt_onlytotals = 0;

typedef struct {
	uint64_t size;
	uint64_t encode_time;
	uint64_t decode_time;
} benchmark_lib_result_t;

typedef struct {
	int count;
	uint64_t disk_size;
	uint64_t raw_size; //difference between this and using `px` is this takes original channel count into account
	uint64_t px;
	int w;
	int h;
	benchmark_lib_result_t libpng;
	benchmark_lib_result_t spng;
	benchmark_lib_result_t stbi;
	benchmark_lib_result_t qoi;
} benchmark_result_t;

void benchmark_print_lib(const char * name, benchmark_result_t res, benchmark_lib_result_t lib) {
	lib.encode_time /= res.count;
	lib.decode_time /= res.count;
	lib.size /= res.count;

	printf("%-5s    %8.1f    %8.1f      %8.2f      %8.2f  %8llu  %7.1f%% %7.1f%% %8.2fx\n", name,
		   lib.decode_time / 1000000.0,
		   lib.encode_time / 1000000.0,
		   lib.decode_time > 0 ? res.px / (lib.decode_time / 1000.0) : 0.0,
		   lib.encode_time > 0 ? res.px / (lib.encode_time / 1000.0) : 0.0,
		   lib.size / 1024,
		   lib.size / (res.px * 4.0) * 100,
		   lib.size / (double) res.raw_size * 100,
		   lib.size / (double) res.disk_size);
}

void benchmark_print_result(benchmark_result_t res) {
	res.px /= res.count;
	res.disk_size /= res.count;
	res.raw_size /= res.count;

	printf("        decode ms   encode ms   decode mpps   encode mpps   size kb   vs rgba   vs raw   vs disk\n");
	if (!opt_nopng) {
	#ifndef _WIN32
		benchmark_print_lib("lpng", res, res.libpng);
	#endif
		benchmark_print_lib("spng", res, res.spng);
		benchmark_print_lib("stbi", res, res.stbi);
	}
	benchmark_print_lib("qoi", res, res.qoi);
	printf("\n");
	fflush(stdout);
}

// Run __VA_ARGS__ a number of times and meassure the time taken. The first
// run is ignored.
#define BENCHMARK_FN(NOWARMUP, RUNS, AVG_TIME, ...) \
	do { \
		uint64_t time = 0; \
		for (int i = NOWARMUP; i <= RUNS; i++) { \
			uint64_t time_start = ns(); \
			__VA_ARGS__ \
			uint64_t time_end = ns(); \
			if (i > 0) { \
				time += time_end - time_start; \
			} \
		} \
		AVG_TIME = time / RUNS; \
	} while (0)

benchmark_result_t benchmark_image(const char *path) {
	int encoded_png_size;
	int encoded_qoi_size;
	int w;
	int h;
	int channels;

	// Load the encoded PNG, encoded QOI and raw pixels into memory
	if(!stbi_info(path, &w, &h, &channels)) {
		ERROR_EXIT("Error decoding header %s", path);
	}

	if (channels != 3) {
		channels = 4;
	}

	void *pixels = (void *)stbi_load(path, &w, &h, NULL, channels);
	void *encoded_png = fload(path, &encoded_png_size);
	qoi_desc qoiDesc = {
		.width = (unsigned) w,
		.height = (unsigned) h,
		.channels = (unsigned char) channels,
		.colorspace = QOI_SRGB
	};
	void *encoded_qoi = qoi_encode(pixels, &qoiDesc, &encoded_qoi_size);

	if (!pixels || !encoded_qoi || !encoded_png) {
		ERROR_EXIT("Error decoding %s\n", path);
	}

	// Verify QOI Output
	if (!opt_noverify) {
		qoi_desc dc;
		void *pixels_qoi = qoi_decode(encoded_qoi, encoded_qoi_size, &dc, channels);
		if (memcmp(pixels, pixels_qoi, w * h * channels) != 0) {
			ERROR_EXIT("QOI roundtrip pixel missmatch for %s", path);
		}
		free(pixels_qoi);
	}

	benchmark_result_t res = {0};
	res.count = 1;
	res.disk_size = encoded_png_size;
	res.raw_size = w * h * channels;
	res.px = w * h;
	res.w = w;
	res.h = h;

	// Decoding
	if (!opt_nodecode) {
		if (!opt_nopng) {
		#ifndef _WIN32
			BENCHMARK_FN(opt_nowarmup, opt_runs, res.libpng.decode_time, {
				int dec_w, dec_h;
				void *dec_p = libpng_decode(encoded_png, encoded_png_size, &dec_w, &dec_h);
				free(dec_p);
			});
		#endif

			BENCHMARK_FN(opt_nowarmup, opt_runs, res.spng.decode_time, {
				void *dec_p = spng_decode(encoded_png, encoded_png_size);
				free(dec_p);
			});

			BENCHMARK_FN(opt_nowarmup, opt_runs, res.stbi.decode_time, {
				int dec_w, dec_h, dec_channels;
				void *dec_p = stbi_load_from_memory((const stbi_uc *) encoded_png, encoded_png_size, &dec_w, &dec_h, &dec_channels, 4);
				free(dec_p);
			});
		}

		BENCHMARK_FN(opt_nowarmup, opt_runs, res.qoi.decode_time, {
			qoi_desc desc;
			void *dec_p = qoi_decode(encoded_qoi, encoded_qoi_size, &desc, 4);
			free(dec_p);
		});
	}

	// Encoding
	if (!opt_noencode) {
		if (!opt_nopng) {
		#ifndef _WIN32
			BENCHMARK_FN(opt_nowarmup, opt_runs, res.libpng.encode_time, {
				int enc_size;
				void *enc_p = libpng_encode(pixels, w, h, channels, &enc_size);
				res.libpng.size = enc_size;
				free(enc_p);
			});
		#endif

			BENCHMARK_FN(opt_nowarmup, opt_runs, res.spng.encode_time, {
				size_t enc_size = 0;
				void *enc_p = spng_encode(pixels, w, h, channels, &enc_size);
				res.spng.size = enc_size;
				free(enc_p);
			});

			BENCHMARK_FN(opt_nowarmup, opt_runs, res.stbi.encode_time, {
				int enc_size = 0;
				stbi_write_png_to_func(stbi_write_callback, &enc_size, w, h, channels, pixels, 0);
				res.stbi.size = enc_size;
			});
		}

		BENCHMARK_FN(opt_nowarmup, opt_runs, res.qoi.encode_time, {
			int enc_size;
			qoi_desc qoiDesc = {
				.width = (unsigned) w,
				.height = (unsigned) h,
				.channels = (unsigned char) channels,
				.colorspace = QOI_SRGB
			};
			void *enc_p = qoi_encode(pixels, &qoiDesc, &enc_size);
			res.qoi.size = enc_size;
			free(enc_p);
		});
	}

	free(pixels);
	free(encoded_png);
	free(encoded_qoi);

	return res;
}

void benchmark_directory(const char *path, List<DirEnt> files, benchmark_result_t *grand_total) {
	if (!opt_norecurse) {
		for (DirEnt & file : files) {
			if (file.isDir) {
				char subpath[1024];
				snprintf(subpath, 1024, "%s/%s", path, file.name);
				benchmark_directory(subpath, file.children, grand_total);
			}
		}
	}

	float total_percentage = 0;
	int total_size = 0;

	benchmark_result_t dir_total = {0};

	int has_shown_heaad = 0;
	for (DirEnt & file : files) {
		if (strcmp(file.name + strlen(file.name) - 4, ".png") != 0) {
			continue;
		}

		if (!has_shown_heaad) {
			has_shown_heaad = 1;
			printf("## Benchmarking %s/*.png -- %d runs\n\n", path, opt_runs);
		}

		char *file_path = (char *) malloc(strlen(file.name) + strlen(path)+8);
		sprintf(file_path, "%s/%s", path, file.name);
		
		benchmark_result_t res = benchmark_image(file_path);

		if (!opt_onlytotals) {
			printf("## %s size: %dx%d (%llu kb)\n", file_path, res.w, res.h, res.disk_size / 1024);
			benchmark_print_result(res);
		}

		free(file_path);

		dir_total.count++;
		dir_total.disk_size += res.disk_size;
		dir_total.raw_size += res.raw_size;
		dir_total.px += res.px;
		dir_total.libpng.encode_time += res.libpng.encode_time;
		dir_total.libpng.decode_time += res.libpng.decode_time;
		dir_total.libpng.size += res.libpng.size;
		dir_total.spng.encode_time += res.spng.encode_time;
		dir_total.spng.decode_time += res.spng.decode_time;
		dir_total.spng.size += res.spng.size;
		dir_total.stbi.encode_time += res.stbi.encode_time;
		dir_total.stbi.decode_time += res.stbi.decode_time;
		dir_total.stbi.size += res.stbi.size;
		dir_total.qoi.encode_time += res.qoi.encode_time;
		dir_total.qoi.decode_time += res.qoi.decode_time;
		dir_total.qoi.size += res.qoi.size;

		grand_total->count++;
		grand_total->disk_size += res.disk_size;
		grand_total->raw_size += res.raw_size;
		grand_total->px += res.px;
		grand_total->libpng.encode_time += res.libpng.encode_time;
		grand_total->libpng.decode_time += res.libpng.decode_time;
		grand_total->libpng.size += res.libpng.size;
		grand_total->spng.encode_time += res.spng.encode_time;
		grand_total->spng.decode_time += res.spng.decode_time;
		grand_total->spng.size += res.spng.size;
		grand_total->stbi.encode_time += res.stbi.encode_time;
		grand_total->stbi.decode_time += res.stbi.decode_time;
		grand_total->stbi.size += res.stbi.size;
		grand_total->qoi.encode_time += res.qoi.encode_time;
		grand_total->qoi.decode_time += res.qoi.decode_time;
		grand_total->qoi.size += res.qoi.size;
	}

	if (dir_total.count > 0) {
		printf("## Total for %s\n", path);
		benchmark_print_result(dir_total);
	}
}

int main(int argc, char **argv) {
	if (argc < 3) {
		printf("Usage: qoibench <iterations> <directory> [options]\n");
		printf("Options:\n");
		printf("    --nowarmup ... don't perform a warmup run\n");
		printf("    --nopng ...... don't run png encode/decode\n");
		printf("    --noverify ... don't verify qoi roundtrip\n");
		printf("    --noencode ... don't run encoders\n");
		printf("    --nodecode ... don't run decoders\n");
		printf("    --norecurse .. don't descend into directories\n");
		printf("    --onlytotals . don't print individual image results\n");
		printf("Examples\n");
		printf("    qoibench 10 images/textures/\n");
		printf("    qoibench 1 images/textures/ --nopng --nowarmup\n");
		exit(1);
	}

	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--nowarmup") == 0) { opt_nowarmup = 1; }
		else if (strcmp(argv[i], "--nopng") == 0) { opt_nopng = 1; }
		else if (strcmp(argv[i], "--noverify") == 0) { opt_noverify = 1; }
		else if (strcmp(argv[i], "--noencode") == 0) { opt_noencode = 1; }
		else if (strcmp(argv[i], "--nodecode") == 0) { opt_nodecode = 1; }
		else if (strcmp(argv[i], "--norecurse") == 0) { opt_norecurse = 1; }
		else if (strcmp(argv[i], "--onlytotals") == 0) { opt_onlytotals = 1; }
		else { ERROR_EXIT("Unknown option %s", argv[i]); }
	}

	opt_runs = atoi(argv[1]);
	if (opt_runs <=0) {
		ERROR_EXIT("Invalid number of runs %d", opt_runs);
	}

	benchmark_result_t grand_total = {0};
	List<DirEnt> files = fetch_dir_info_recursive(argv[2]);
	benchmark_directory(argv[2], files, &grand_total);

	if (grand_total.count > 0) {
		printf("# Grand total for %s\n", argv[2]);
		benchmark_print_result(grand_total);
	}
	else {
		printf("No images found in %s\n", argv[2]);
	}

	printf("\nHistogram of %llu indices (%4.2f%% of all pixels):\n",
		index_total, (double) index_total / (double) pixel_total * 100.0);
	for (int i = 0; i < 64; ++i) {
		printf("%2d: %4.2f%%\n", i, (double) index_histogram[i] / (double) index_total * 100.0);
	}

	return 0;
}
