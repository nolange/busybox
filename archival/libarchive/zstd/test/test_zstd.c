// -fmerge-all-constants
#include "libbb.h"

#include "bb_archive.h"
int *const bb_errno;

void FAST_FUNC bb_error_msg(const char *s, ...)
{

}

void FAST_FUNC bb_simple_perror_msg(const char *s)
{
}


void FAST_FUNC bb_die_memory_exhausted(void)
{
        abort();
}
void FAST_FUNC bb_simple_error_msg(const char *s)
{
}

// Die if we can't allocate size bytes of memory.
void* FAST_FUNC xmalloc(size_t size)
{
        void *ptr = malloc(size);
        if (ptr == NULL && size != 0)
                bb_die_memory_exhausted();
        return ptr;
}



ssize_t FAST_FUNC safe_read(int fd, void *buf, size_t count)
{
        ssize_t n;

        for (;;) {
                n = read(fd, buf, count);
                if (n >= 0 || errno != EINTR)
                        break;
                /* Some callers set errno=0, are upset when they see EINTR.
                 * Returning EINTR is wrong since we retry read(),
                 * the "error" was transient.
                 */
                errno = 0;
                /* repeat the read() */
        }

        return n;
}


ssize_t FAST_FUNC transformer_write(transformer_state_t *xstate, const void *buf, size_t bufsize)
{
        ssize_t nwrote;

        if (xstate->mem_output_size_max != 0) {
                size_t pos = xstate->mem_output_size;
                size_t size;

                size = (xstate->mem_output_size += bufsize);
                if (size > xstate->mem_output_size_max) {
                        free(xstate->mem_output_buf);
                        xstate->mem_output_buf = NULL;
                        abort();
                        nwrote = -1;
                        goto ret;
                }
                xstate->mem_output_buf = realloc(xstate->mem_output_buf, size + 1);
                memcpy(xstate->mem_output_buf + pos, buf, bufsize);
                xstate->mem_output_buf[size] = '\0';
                nwrote = bufsize;
        } else {
                nwrote = write(xstate->dst_fd, buf, bufsize);
                if (nwrote != (ssize_t)bufsize) {
                        abort();
                        nwrote = -1;
                        goto ret;
                }
        }
 ret:
        return nwrote;
}

ssize_t FAST_FUNC xtransformer_write(transformer_state_t *xstate, const void *buf, size_t bufsize)
{
        ssize_t nwrote = transformer_write(xstate, buf, bufsize);
        if (nwrote != (ssize_t)bufsize) {
                abort();
        }
        return nwrote;
}


int main(int argc, char const *argv[])
{
    int fdf = open(argv[1], O_RDONLY);
    int fdt = open(argv[2], O_WRONLY | O_CREAT, 0644);

    transformer_state_t tstate = {};
    tstate.src_fd = fdf;
    tstate.dst_fd = fdt;

    unpack_zstd_stream(&tstate);
#ifdef ZSTD_COMPRESS
    compress_stream_zstd(fdf, fdt, ~(uint64_t)0);
#endif
    /* code */
    return 0;
}
