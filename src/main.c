#include "bfjit.h"
#include "bfopt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bf_read_file(const char *path, char **contents, size_t *length) {
    FILE *stream;
    long file_size;
    char *buf;
    size_t read_count;

    stream = fopen(path, "rb");
    if (stream == NULL) {
        return 0;
    }

    if (fseek(stream, 0, SEEK_END) != 0) {
        fclose(stream);
        return 0;
    }

    file_size = ftell(stream);
    if (file_size < 0) {
        fclose(stream);
        return 0;
    }

    if (fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return 0;
    }

    buf = malloc((size_t)file_size + 1);
    if (buf == NULL) {
        fclose(stream);
        return 0;
    }

    read_count = fread(buf, 1, (size_t)file_size, stream);
    fclose(stream);
    if (read_count != (size_t)file_size) {
        free(buf);
        return 0;
    }

    buf[file_size] = '\0';
    *contents = buf;
    *length = (size_t)file_size;
    return 1;
}

int main(int argc, char **argv) {
    static const size_t tape_size = 30000;
    bf_program program;
    bf_parse_err parse_err;
    bf_jit_err jit_err;
    uint8_t *tape;
    char *src;
    size_t src_length;
    int exit_code;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <program.bf>\n", argv[0]);
        return 1;
    }

    if (!bf_read_file(argv[1], &src, &src_length)) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return 1;
    }

    if (!bf_parse_src(src, src_length, &program, &parse_err)) {
        fprintf(stderr, "parse err at %zu:%zu: %s\n", parse_err.loc.line,
                parse_err.loc.column, parse_err.msg);
        free(src);
        return 1;
    }

    bf_opt_program(&program);

    tape = calloc(tape_size, sizeof(*tape));
    if (tape == NULL) {
        fprintf(stderr, "failed to allocate tape\n");
        bf_program_dispose(&program);
        free(src);
        return 1;
    }

    exit_code = 0;
    if (!bf_jit_execute_program(&program, tape, tape_size, &jit_err)) {
        fprintf(stderr, "jit err: %s\n", jit_err.msg);
        exit_code = 1;
    }

    free(tape);
    bf_program_dispose(&program);
    free(src);
    return exit_code;
}
