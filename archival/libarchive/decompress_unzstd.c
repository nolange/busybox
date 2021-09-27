/* vi: set sw=4 ts=4: */
/*
 * Glue for zstd decompression
 * Copyright (C) 2021 Norbert Lange <nolange79@gmail.com>
 *
 * Based on compress.c from the systemd project,
 * provided by Norbert Lange <nolange79@gmail.com>.
 * Which originally was copied from the streaming_decompression.c
 * example from the zstd project, written by Yann Collet 
 * 
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */

#include "libbb.h"
#include "bb_archive.h"

/* start with macros configuring zstd */

#define DEBUGLEVEL 0
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_LIB_DEPRECATED 0
#define ZSTD_NO_UNUSED_FUNCTIONS 1
#define ZSTD_STRIP_ERROR_STRINGS 1
#define ZSTD_TRACE 0
#define ZSTD_DECOMPRESS_DICTIONARY 0
#define ZSTD_DECOMPRESS_MULTIFRAME 0
#define XXH_PRIVATE_API 1

/* complete API as static functions */
#define ZSTDLIB_VISIBILITY MEM_STATIC
#define ZSTDERRORLIB_VISIBILITY MEM_STATIC

// NO_PREFETCH does something with ZSTD_FORCE_DECOMPRESS_SEQUENCES_LONG
// at which point the added size is negligible

#if CONFIG_FEATURE_ZSTD_SMALL >= 9
#define ZSTD_NO_INLINE 1
#endif

// falsch - bloatcheck misst nicht alles!
// 0 39953 2.95s
// 4 24196 3.51s
// 5 18713 3.60s
// 7 15236 3.86s
// 8 18949 4.10s
// 9 17562 4.67s

// make O=/tmp/build all bloatcheck && for i in 1 2 3 4; do time /tmp/build/busybox unzstd -c > /dev/null /tmp/bullseye-xfce.tar.zst; done

#if CONFIG_FEATURE_ZSTD_SMALL >= 7
#define HUF_FORCE_DECOMPRESS_X1 1
#define ZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT 1
#elif CONFIG_FEATURE_ZSTD_SMALL >= 5
#define HUF_FORCE_DECOMPRESS_X1 1
#endif

#if CONFIG_FEATURE_ZSTD_SMALL <= 7
/* doesnt blow up code too much, -O3 is horrible */
#ifdef __GNUC__
#pragma GCC optimize ("O2")
#endif
#endif

#if CONFIG_FEATURE_ZSTD_SMALL > 0
/* no dynamic detection of bmi2 instruction,
 * prefer using CFLAGS setting to -march=haswell or similar */
# if !defined(__BMI2__)
#  define DYNAMIC_BMI2 0
# endif
#endif

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* Include zstd_deps.h first with all the options we need enabled. */
#define ZSTD_DEPS_NEED_MALLOC
#define ZSTD_DEPS_NEED_MATH64
#define ZSTD_DEPS_NEED_STDINT
#include "zstd/common/zstd_deps.h"

#include "zstd/common/entropy_common.c"
#include "zstd/common/fse_decompress.c"

typedef struct { void* customAlloc; void* customFree; void* opaque; } ZSTD_customMem;
#define ZSTD_defaultCMem ((ZSTD_customMem){NULL, NULL, NULL})
static inline void* ZSTD_customMalloc(size_t size, ZSTD_customMem customMem) {
	(void)customMem;
	return malloc(size);
}
static inline void ZSTD_customFree(void* ptr, ZSTD_customMem customMem) {
	(void)customMem;
	free(ptr);
}

#include "zstd/common/zstd_internal.h"

ZSTDLIB_API unsigned    ZSTD_isError(size_t code);
ZSTDLIB_API const char* ZSTD_getErrorName(size_t code);

#include "zstd/decompress/huf_decompress.c"
#include "zstd/zstd_ddict_noops.c"
#include "zstd/decompress/zstd_decompress.c"
#include "zstd/decompress/zstd_decompress_block.c"

ALWAYS_INLINE static size_t roundupsize(size_t size, size_t align)
{
	return (size + align - 1U) & ~(align - 1);
}

ALWAYS_INLINE static IF_DESKTOP(long long) int
unpack_zstd_stream_inner(transformer_state_t *xstate,
	ZSTD_DStream *dctx, void *out_buff)
{
	const U32 zstd_magic = ZSTD_MAGIC;
	const size_t in_allocsize = roundupsize(ZSTD_DStreamInSize(), 1024),
		out_allocsize = roundupsize(ZSTD_DStreamOutSize(), 1024);

	IF_DESKTOP(long long int total = 0;)
	size_t last_result = ZSTD_error_maxCode + 1;
	unsigned input_fixup;
	void *in_buff = (char *)out_buff + out_allocsize;

	memcpy(in_buff, &zstd_magic, 4);
	input_fixup = xstate->signature_skipped ? 4 : 0;

	/* This loop assumes that the input file is one or more concatenated
	 * zstd streams. This example won't work if there is trailing non-zstd
	 * data at the end, but streaming decompression in general handles this
	 * case. ZSTD_decompressStream() returns 0 exactly when the frame is
	 * completed, and doesn't consume input after the frame.
	 */
	for (;;) {
		bool has_error = false;
		ZSTD_inBuffer input;
		ssize_t red;

		red = safe_read(xstate->src_fd, (char *)in_buff + input_fixup, in_allocsize - input_fixup);
		if (red < 0) {
			bb_simple_perror_msg(bb_msg_read_error);
			return -1;
		}
		if (red == 0) {
			break;
		}

		input.src = in_buff;
		input.size = (size_t)red + input_fixup;
		input.pos = 0;
		input_fixup = 0;

		/* Given a valid frame, zstd won't consume the last byte of the
		 * frame until it has flushed all of the decompressed data of
		 * the frame. So input.pos < input.size means frame is not done
		 * or there is still output available.
		 */
		while (input.pos < input.size) {
			ZSTD_outBuffer output = { out_buff, out_allocsize, 0 };
			/* The return code is zero if the frame is complete, but
			 * there may be multiple frames concatenated together.
			 * Zstd will automatically reset the context when a
			 * frame is complete. Still, calling ZSTD_DCtx_reset()
			 * can be useful to reset the context to a clean state,
			 * for instance if the last decompression call returned
			 * an error.
			 */
			last_result = ZSTD_decompressStream(dctx, &output, &input);
			if (ZSTD_isError(last_result)) {
				has_error = true;
				break;
			}

			xtransformer_write(xstate, output.dst, output.pos);
			IF_DESKTOP(total += output.pos;)
		}
		if (has_error)
			break;
	}

	if (last_result != 0) {
		/* The last return value from ZSTD_decompressStream did not end
		 * on a frame, but we reached the end of the file! We assume
		 * this is an error, and the input was truncated.
		 */
		if (last_result == ZSTD_error_maxCode + 1) {
			bb_simple_error_msg("could not read zstd data");
		} else {
#if defined(ZSTD_STRIP_ERROR_STRINGS) && ZSTD_STRIP_ERROR_STRINGS == 1
			bb_error_msg("zstd decoder error: %u", (unsigned)last_result);
#else
			bb_error_msg("zstd decoder error: %s", ZSTD_getErrorName(last_result));
#endif
		}
		return -1;
	}

	return IF_DESKTOP(total) + 0;
}

IF_DESKTOP(long long) int FAST_FUNC
unpack_zstd_stream(transformer_state_t *xstate)
{
	const size_t in_allocsize = roundupsize(ZSTD_DStreamInSize(), 1024),
		   out_allocsize = roundupsize(ZSTD_DStreamOutSize(), 1024);

	IF_DESKTOP(long long) int result;
	void *out_buff;
	ZSTD_DStream *dctx;

	dctx = ZSTD_createDStream();
	if (!dctx) {
		/* should be the only possibly reason of failure */
		bb_die_memory_exhausted();
	}

	out_buff = xmalloc(in_allocsize + out_allocsize);

	result = unpack_zstd_stream_inner(xstate, dctx, out_buff);
	free(out_buff);
	ZSTD_freeDStream(dctx);
	return result;
}

