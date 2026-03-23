#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <source.b>\n", argv[0]);
        return 1;
    }

    FILE *source_file = fopen(argv[1], "rb");
    if (source_file == NULL) {
        perror("fopen");
        return 1;
    }

    if (fseek(source_file, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(source_file);
        return 1;
    }

    long source_size = ftell(source_file);
    if (source_size < 0) {
        perror("ftell");
        fclose(source_file);
        return 1;
    }

    if (fseek(source_file, 0, SEEK_SET) != 0) {
        perror("fseek");
        fclose(source_file);
        return 1;
    }

    char *src = malloc((size_t)source_size + 1);
    if (src == NULL) {
        perror("malloc");
        fclose(source_file);
        return 1;
    }

    size_t bytes_read = fread(src, 1, (size_t)source_size, source_file);
    if (bytes_read != (size_t)source_size) {
        perror("fread");
        free(src);
        fclose(source_file);
        return 1;
    }
    src[source_size] = '\0';

    fclose(source_file);

    size_t program_size = (size_t)source_size;
    size_t *brackets = calloc(program_size, sizeof(size_t));
    size_t *stack = malloc(program_size * sizeof(size_t));
    if (brackets == NULL || stack == NULL) {
        perror("malloc");
        free(brackets);
        free(stack);
        free(src);
        return 1;
    }

    size_t stack_size = 0;
    for (size_t i = 0; i < program_size; ++i) {
        if (src[i] == '[') {
            stack[stack_size++] = i;
        } else if (src[i] == ']') {
            size_t j = stack[--stack_size];
            brackets[j] = i;
            brackets[i] = j;
        }
    }

    uint8_t mem[65536] = {0};
    size_t dp = 0;
    size_t ip = 0;

    while (ip < program_size) {
        switch (src[ip]) {
        case '>':
            ++dp;
            break;
        case '<':
            --dp;
            break;
        case '+':
            ++mem[dp];
            break;
        case '-':
            --mem[dp];
            break;
        case '.':
            fputc(mem[dp], stdout);
            break;
        case ',': {
            int ch = fgetc(stdin);
            mem[dp] = (uint8_t)ch;
            break;
        }
        case '[':
            if (mem[dp] == 0) {
                ip = brackets[ip];
            }
            break;
        case ']':
            if (mem[dp] != 0) {
                ip = brackets[ip];
            }
            break;
        }
        ++ip;
    }

    free(stack);
    free(brackets);
    free(src);
    return 0;
}
