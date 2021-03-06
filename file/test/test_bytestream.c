#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include <bytestream.h>

/******************************************************************************/
/* Byte Stream File Test */
/******************************************************************************/

int test_bytestream(FILE *in, int (*stream_init)(struct ByteStream *self), int (*stream_close)(struct ByteStream *self), int (*stream_read)(struct ByteStream *self, uint8_t *data, uint32_t *address)) {
    struct ByteStream os;
    uint8_t data;
    uint32_t address;
    int i, ret;
    int success = 1;

    /* Setup the Byte Stream */
    os.in = in;
    os.stream_init = stream_init;
    os.stream_close = stream_close;
    os.stream_read = stream_read;

    printf("Running test_bytestream()\n\n");

    /* Initialize the stream */
    ret = os.stream_init(&os);
    printf("os.stream_init(): %d\n", ret);
    if (ret < 0) {
        printf("\tError: %s\n\n", os.error);
        return -1;
    }
    printf("\n");

    /* Read 32 bytes */
    for (i = 0; i < 32; i++) {
        ret = os.stream_read(&os, &data, &address);
        printf("os.stream_read(): %d\n", ret);
        if (ret == STREAM_EOF)
            break;
        else if (ret < 0) {
            printf("\tError: %s\n\n", os.error);
            success = 0;
            break;
        }
        printf("\t%08x:%02x\n", address, data);
    }

    /* Close the stream */
    ret = os.stream_close(&os);
    printf("os.stream_close(): %d\n", ret);
    if (ret < 0) {
        printf("\tError: %s\n\n", os.error);
        return -1;
    }

    printf("\n");

    if (success)
        return 0;

    return -1;
}

