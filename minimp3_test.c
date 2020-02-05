/*#define MINIMP3_ONLY_MP3*/
/*#define MINIMP3_ONLY_SIMD*/
/*#define MINIMP3_NONSTANDARD_BUT_LOGICAL*/
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ALLOW_MONO_STEREO_TRANSITION
#include "minimp3_ex.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <time.h>
#if defined(_MSC_VER)
    #define strcasecmp(str1, str2) _strnicmp(str1, str2, strlen(str2))
#else
    #include <strings.h>
#endif

#define MODE_LOAD    0
#define MODE_ITERATE 1
#define MODE_STREAM  2

static int16_t read16le(const void *p)
{
    const uint8_t *src = (const uint8_t *)p;
    return ((src[0]) << 0) | ((src[1]) << 8);
}

#ifndef MINIMP3_NO_WAV
static char *wav_header(int hz, int ch, int bips, int data_bytes)
{
    static char hdr[44] = "RIFFsizeWAVEfmt \x10\0\0\0\1\0ch_hz_abpsbabsdatasize";
    unsigned long nAvgBytesPerSec = bips*ch*hz >> 3;
    unsigned int nBlockAlign      = bips*ch >> 3;

    *(int32_t *)(void*)(hdr + 0x04) = 44 + data_bytes - 8;   /* File size - 8 */
    *(int16_t *)(void*)(hdr + 0x14) = 1;                     /* Integer PCM format */
    *(int16_t *)(void*)(hdr + 0x16) = ch;
    *(int32_t *)(void*)(hdr + 0x18) = hz;
    *(int32_t *)(void*)(hdr + 0x1C) = nAvgBytesPerSec;
    *(int16_t *)(void*)(hdr + 0x20) = nBlockAlign;
    *(int16_t *)(void*)(hdr + 0x22) = bips;
    *(int32_t *)(void*)(hdr + 0x28) = data_bytes;
    return hdr;
}
#endif

static unsigned char *preload(FILE *file, int *data_size)
{
    unsigned char *data;
    *data_size = 0;
    if (!file)
        return 0;
    if (fseek(file, 0, SEEK_END))
        return 0;
    *data_size = (int)ftell(file);
    if (*data_size < 0)
        return 0;
    if (fseek(file, 0, SEEK_SET))
        return 0;
    data = (unsigned char*)malloc(*data_size);
    if (!data)
        return 0;
    if ((int)fread(data, 1, *data_size, file) != *data_size)
        exit(1);
    return data;
}

typedef struct
{
    mp3dec_t *mp3d;
    mp3dec_file_info_t *info;
    size_t allocated;
} frames_iterate_data;

static int frames_iterate_cb(void *user_data, const uint8_t *frame, int frame_size, int free_format_bytes, size_t buf_size, size_t offset, mp3dec_frame_info_t *info)
{
    (void)buf_size;
    (void)offset;
    (void)free_format_bytes;
    frames_iterate_data *d = user_data;
    d->info->channels = info->channels;
    d->info->hz       = info->hz;
    d->info->layer    = info->layer;
    /*printf("%d %d %d\n", frame_size, (int)offset, info->channels);*/
    if ((d->allocated - d->info->samples*sizeof(mp3d_sample_t)) < MINIMP3_MAX_SAMPLES_PER_FRAME*sizeof(mp3d_sample_t))
    {
        if (!d->allocated)
            d->allocated = 1024*1024;
        else
            d->allocated *= 2;
        d->info->buffer = realloc(d->info->buffer, d->allocated);
    }
    int samples = mp3dec_decode_frame(d->mp3d, frame, frame_size, d->info->buffer + d->info->samples, info);
    if (samples)
    {
        d->info->samples += samples*info->channels;
    }
    return 0;
}

static void decode_file(const char *input_file_name, const unsigned char *buf_ref, int ref_size, FILE *file_out, const int wave_out, const int mode, int position)
{
    mp3dec_t mp3d;
    int i, res = -1, data_bytes, total_samples = 0, maxdiff = 0;
    double MSE = 0.0, psnr;

    mp3dec_file_info_t info;
    memset(&info, 0, sizeof(info));
    if (MODE_LOAD == mode)
    {
        res = mp3dec_load(&mp3d, input_file_name, &info, 0, 0);
    } else if (MODE_ITERATE == mode)
    {
        frames_iterate_data d = { &mp3d, &info, 0 };
        mp3dec_init(&mp3d);
        res = mp3dec_iterate(input_file_name, frames_iterate_cb, &d);
    } else if (MODE_STREAM == mode)
    {
        mp3dec_ex_t dec;
        size_t readed;
        res = mp3dec_ex_open(&dec, input_file_name, MP3D_SEEK_TO_SAMPLE);
        info.samples = dec.samples;
        info.buffer  = malloc(dec.samples*sizeof(int16_t));
        info.hz      = dec.info.hz;
        info.layer   = dec.info.layer;
        info.channels = dec.info.channels;
        if (position < 0)
        {
            srand(time(0));
            position = info.samples > 150 ? (uint64_t)(info.samples - 150)*rand()/RAND_MAX : 0;
            printf("info: seek to %d/%d\n", position, (int)info.samples);
        }
        if (position)
        {
            info.samples -= MINIMP3_MIN(info.samples, (size_t)position);
            int skip_ref = MINIMP3_MIN((size_t)ref_size, position*sizeof(int16_t));
            buf_ref  += skip_ref;
            ref_size -= skip_ref;
            mp3dec_ex_seek(&dec, position);
        }
        readed = mp3dec_ex_read(&dec, info.buffer, info.samples);
        if (readed != info.samples)
        {
            printf("error: mp3dec_ex_read() readed less than expected\n");
            exit(1);
        }
        readed = mp3dec_ex_read(&dec, info.buffer, 1);
        if (readed)
        {
            printf("error: mp3dec_ex_read() readed more than expected\n");
            exit(1);
        }
        mp3dec_ex_close(&dec);
    } else
    {
        printf("error: unknown mode\n");
        exit(1);
    }
    if (res)
    {
        if (ref_size)
        {
            printf("error: file not found or read error\n");
            exit(1);
        }
    }
#ifdef MINIMP3_FLOAT_OUTPUT
    int16_t *buffer = malloc(info.samples*sizeof(int16_t));
    mp3dec_f32_to_s16(info.buffer, buffer, info.samples);
    free(info.buffer);
#else
    int16_t *buffer = info.buffer;
#endif
#ifndef MINIMP3_NO_WAV
    if (wave_out && file_out)
        fwrite(wav_header(0, 0, 0, 0), 1, 44, file_out);
#endif
    if (info.samples)
    {
        total_samples += info.samples;
        if (buf_ref)
        {
            size_t ref_samples = ref_size/2;
            if (ref_samples != info.samples && (ref_samples + 1152) != info.samples && (ref_samples + 2304) != info.samples && 3 == info.layer)
            {   /* some standard vectors are for some reason a little shorter */
                printf("error: reference and produced number of samples do not match (%d/%d)\n", (int)ref_samples, (int)info.samples);
                exit(1);
            }
            int max_samples = MINIMP3_MIN(ref_samples, info.samples);
            for (i = 0; i < max_samples; i++)
            {
                int MSEtemp = abs((int)buffer[i] - (int)(int16_t)read16le(&buf_ref[i*sizeof(int16_t)]));
                if (MSEtemp > maxdiff)
                    maxdiff = MSEtemp;
                MSE += (float)MSEtemp*(float)MSEtemp;
            }
        }
        if (file_out)
            fwrite(buffer, info.samples, sizeof(int16_t), file_out);
        free(buffer);
    }

#ifndef LIBFUZZER
    MSE /= total_samples ? total_samples : 1;
    if (0 == MSE)
        psnr = 99.0;
    else
        psnr = 10.0*log10(((double)0x7fff*0x7fff)/MSE);
    printf("rate=%d samples=%d max_diff=%d PSNR=%f\n", info.hz, total_samples, maxdiff, psnr);
    if (psnr < 96)
    {
        printf("error: PSNR compliance failed\n");
        exit(1);
    }
#endif
#ifndef MINIMP3_NO_WAV
    if (wave_out && file_out)
    {
        data_bytes = ftell(file_out) - 44;
        rewind(file_out);
        fwrite(wav_header(info.hz, info.channels, 16, data_bytes), 1, 44, file_out);
    }
#endif
#ifdef MP4_MODE
    if (!total_samples)
    {
        printf("error: mp4 test should decode some samples\n");
        exit(1);
    }
#endif
}

#ifdef LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size)
{
    decode_file(Data, Size, 0, 0, 0, 0);
    return 0;
}
#else

#if defined(__arm__) || defined(__aarch64__) || defined(__PPC__)
int main2(int argc, char *argv[]);
int main2(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
    int wave_out = 0, mode = 0, position = 0, i, ref_size;
    for(i = 1; i < argc; i++)
    {
        if (argv[i][0] != '-')
            break;
        switch (argv[i][1])
        {
        case 'm': i++; if (i < argc) mode = atoi(argv[i]); break;
        case 's': i++; if (i < argc) position = atoi(argv[i]); break;
        default:
            printf("error: unrecognized option\n");
            return 1;
        }
    }
    char *ref_file_name    = (argc > (i + 1)) ? argv[i + 1] : NULL;
    char *output_file_name = (argc > (i + 2)) ? argv[i + 2] : NULL;
    FILE *file_out = NULL;
    if (output_file_name)
    {
        file_out = fopen(output_file_name, "wb");
#ifndef MINIMP3_NO_WAV
        char *ext = strrchr(output_file_name, '.');
        if (ext && !strcasecmp(ext + 1, "wav"))
            wave_out = 1;
#endif
    }
    FILE *file_ref = ref_file_name ? fopen(ref_file_name, "rb") : NULL;
    unsigned char *buf_ref = preload(file_ref, &ref_size);
    if (file_ref)
        fclose(file_ref);
#ifdef __AFL_HAVE_MANUAL_CONTROL
    __AFL_INIT();
    while (__AFL_LOOP(1000)) {
#endif
    char *input_file_name  = (argc > i) ? argv[i] : NULL;
    if (!input_file_name)
    {
        printf("error: no file names given\n");
        return 1;
    }
    decode_file(input_file_name, buf_ref, ref_size, file_out, wave_out, mode, position);
#ifdef __AFL_HAVE_MANUAL_CONTROL
    }
#endif
    if (buf_ref)
        free(buf_ref);
    if (file_out)
        fclose(file_out);
    return 0;
}

#if defined(__arm__) || defined(__aarch64__) || defined(__PPC__)
static const char *g_files[] = {
    "vectors/ILL2_center2.bit",
    "vectors/ILL2_dual.bit",
    "vectors/ILL2_dynx22.bit",
    "vectors/ILL2_dynx31.bit",
    "vectors/ILL2_dynx32.bit",
    "vectors/ILL2_ext_switching.bit",
    "vectors/ILL2_layer1.bit",
    "vectors/ILL2_layer3.bit",
    "vectors/ILL2_mono.bit",
    "vectors/ILL2_multilingual.bit",
    "vectors/ILL2_overalloc1.bit",
    "vectors/ILL2_overalloc2.bit",
    "vectors/ILL2_prediction.bit",
    "vectors/ILL2_samples.bit",
    "vectors/ILL2_scf63.bit",
    "vectors/ILL2_tca21.bit",
    "vectors/ILL2_tca30.bit",
    "vectors/ILL2_tca30_PC.bit",
    "vectors/ILL2_tca31_mtx0.bit",
    "vectors/ILL2_tca31_mtx2.bit",
    "vectors/ILL2_tca31_PC.bit",
    "vectors/ILL2_tca32_PC.bit",
    "vectors/ILL2_wrongcrc.bit",
    "vectors/ILL4_ext_id1.bit",
    "vectors/ILL4_sync.bit",
    "vectors/ILL4_wrongcrc.bit",
    "vectors/ILL4_wrong_length1.bit",
    "vectors/ILL4_wrong_length2.bit",
    "vectors/l1-fl1.bit",
    "vectors/l1-fl2.bit",
    "vectors/l1-fl3.bit",
    "vectors/l1-fl4.bit",
    "vectors/l1-fl5.bit",
    "vectors/l1-fl6.bit",
    "vectors/l1-fl7.bit",
    "vectors/l1-fl8.bit",
    "vectors/l2-fl10.bit",
    "vectors/l2-fl11.bit",
    "vectors/l2-fl12.bit",
    "vectors/l2-fl13.bit",
    "vectors/l2-fl14.bit",
    "vectors/l2-fl15.bit",
    "vectors/l2-fl16.bit",
    "vectors/l2-nonstandard-fl1_fl2_ff.bit",
    "vectors/l2-nonstandard-free_format.bit",
    "vectors/l2-nonstandard-test32-size.bit",
    "vectors/l2-test32.bit",
    "vectors/l3-compl.bit",
    "vectors/l3-he_32khz.bit",
    "vectors/l3-he_44khz.bit",
    "vectors/l3-he_48khz.bit",
    "vectors/l3-hecommon.bit",
    "vectors/l3-he_free.bit",
    "vectors/l3-he_mode.bit",
    "vectors/l3-nonstandard-big-iscf.bit",
    "vectors/l3-nonstandard-compl-sideinfo-bigvalues.bit",
    "vectors/l3-nonstandard-compl-sideinfo-blocktype.bit",
    "vectors/l3-nonstandard-compl-sideinfo-size.bit",
    "vectors/l3-nonstandard-sideinfo-size.bit",
    "vectors/l3-si.bit",
    "vectors/l3-si_block.bit",
    "vectors/l3-si_huff.bit",
    "vectors/l3-sin1k0db.bit",
    "vectors/l3-test45.bit",
    "vectors/l3-test46.bit",
    "vectors/M2L3_bitrate_16_all.bit",
    "vectors/M2L3_bitrate_22_all.bit",
    "vectors/M2L3_bitrate_24_all.bit",
    "vectors/M2L3_compl24.bit",
    "vectors/M2L3_noise.bit"
};
int main()
{
    size_t i;
    char buf[256];
    char *v[3];
    v[2] = buf;
    for (i = 0; i < sizeof(g_files)/sizeof(g_files[0]); i++)
    {
        int ret;
        const char *file = g_files[i];
        size_t len = strlen(file);
        strcpy(buf, file);
        buf[len - 3] = 'p';
        buf[len - 2] = 'c';
        buf[len - 1] = 'm';
        v[1] = (char*)file;
        printf("%s\n", file);
        ret = main2(3, v);
        if (ret)
            return ret;
    }
    return 0;
}
#endif
#endif
