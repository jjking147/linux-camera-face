#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FL_FACE_W 80
#define FL_FACE_H 80
#define FL_GRID_X 8
#define FL_GRID_Y 8
#define FL_HIST_BINS 256
#define FL_LABEL_MAX 32
#define FL_MAGIC "FLDBv1\0"
#define FL_VERSION 1u

#define FL_DIM (FL_GRID_X * FL_GRID_Y * FL_HIST_BINS)

typedef struct
{
    int w;
    int h;
    uint8_t *data;
} GrayImage;

typedef struct
{
    char label[FL_LABEL_MAX];
    float *feat;
} DbRecord;

typedef struct
{
    uint32_t dim;
    uint32_t count;
    DbRecord *records;
} FaceDb;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s enroll <db.bin> <label> <img1.pgm> [img2.pgm ...]\n"
            "  %s predict <db.bin> <img.pgm> [threshold]\n"
            "  %s verify <db.bin> <label> <img.pgm> [threshold]\n"
            "  %s list <db.bin>\n\n"
            "Notes:\n"
            "  1) Input image should be a cropped face ROI in PGM format (P5/P2).\n"
            "  2) This is a lightweight LBPH recognizer for embedded boards.\n",
            prog, prog, prog, prog);
}

static void free_image(GrayImage *img)
{
    if (!img)
    {
        return;
    }
    free(img->data);
    img->data = NULL;
    img->w = 0;
    img->h = 0;
}

static int skip_ws_and_comments(FILE *fp)
{
    int c;

    while ((c = fgetc(fp)) != EOF)
    {
        if (isspace(c))
        {
            continue;
        }
        if (c == '#')
        {
            while ((c = fgetc(fp)) != EOF && c != '\n')
            {
            }
            continue;
        }
        ungetc(c, fp);
        return 0;
    }

    return -1;
}

static int read_int_token(FILE *fp, int *out)
{
    if (skip_ws_and_comments(fp) < 0)
    {
        return -1;
    }
    return fscanf(fp, "%d", out) == 1 ? 0 : -1;
}

static int load_pgm(const char *path, GrayImage *img)
{
    FILE *fp = NULL;
    char magic[3] = {0};
    int w;
    int h;
    int maxv;
    size_t need;

    memset(img, 0, sizeof(*img));

    fp = fopen(path, "rb");
    if (!fp)
    {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (fscanf(fp, "%2s", magic) != 1)
    {
        fprintf(stderr, "read magic from %s failed\n", path);
        fclose(fp);
        return -1;
    }

    if (strcmp(magic, "P5") != 0 && strcmp(magic, "P2") != 0)
    {
        fprintf(stderr, "%s is not a PGM (P5/P2)\n", path);
        fclose(fp);
        return -1;
    }

    if (read_int_token(fp, &w) < 0 || read_int_token(fp, &h) < 0 ||
        read_int_token(fp, &maxv) < 0)
    {
        fprintf(stderr, "read header from %s failed\n", path);
        fclose(fp);
        return -1;
    }

    if (w <= 0 || h <= 0 || maxv <= 0 || maxv > 255)
    {
        fprintf(stderr, "invalid pgm header in %s\n", path);
        fclose(fp);
        return -1;
    }

    need = (size_t)w * (size_t)h;
    img->data = (uint8_t *)malloc(need);
    if (!img->data)
    {
        fprintf(stderr, "malloc image failed\n");
        fclose(fp);
        return -1;
    }

    img->w = w;
    img->h = h;

    if (strcmp(magic, "P5") == 0)
    {
        int c;
        do
        {
            c = fgetc(fp);
        } while (c != EOF && isspace(c));
        if (c != EOF)
        {
            ungetc(c, fp);
        }

        if (fread(img->data, 1, need, fp) != need)
        {
            fprintf(stderr, "read pixel data from %s failed\n", path);
            free_image(img);
            fclose(fp);
            return -1;
        }
    }
    else
    {
        size_t i;
        for (i = 0; i < need; ++i)
        {
            int v;
            if (read_int_token(fp, &v) < 0)
            {
                fprintf(stderr, "read ascii pixel from %s failed\n", path);
                free_image(img);
                fclose(fp);
                return -1;
            }
            if (v < 0)
            {
                v = 0;
            }
            if (v > maxv)
            {
                v = maxv;
            }
            img->data[i] = (uint8_t)((v * 255) / maxv);
        }
    }

    fclose(fp);
    return 0;
}

static void hist_equalize(uint8_t *data, int w, int h)
{
    uint32_t hist[256] = {0};
    uint32_t cdf[256];
    uint32_t n = (uint32_t)(w * h);
    uint32_t i;
    uint32_t c = 0;
    uint32_t cdf_min = 0;

    for (i = 0; i < n; ++i)
    {
        hist[data[i]]++;
    }

    for (i = 0; i < 256; ++i)
    {
        c += hist[i];
        cdf[i] = c;
        if (cdf_min == 0 && cdf[i] != 0)
        {
            cdf_min = cdf[i];
        }
    }

    if (n <= cdf_min)
    {
        return;
    }

    for (i = 0; i < n; ++i)
    {
        uint32_t v = data[i];
        uint32_t num = cdf[v] - cdf_min;
        uint32_t den = n - cdf_min;
        data[i] = (uint8_t)((num * 255) / den);
    }
}

static int resize_nn(const GrayImage *src, GrayImage *dst, int out_w, int out_h)
{
    int x;
    int y;

    dst->w = out_w;
    dst->h = out_h;
    dst->data = (uint8_t *)malloc((size_t)out_w * (size_t)out_h);
    if (!dst->data)
    {
        return -1;
    }

    for (y = 0; y < out_h; ++y)
    {
        int sy = (y * src->h) / out_h;
        if (sy >= src->h)
        {
            sy = src->h - 1;
        }
        for (x = 0; x < out_w; ++x)
        {
            int sx = (x * src->w) / out_w;
            if (sx >= src->w)
            {
                sx = src->w - 1;
            }
            dst->data[y * out_w + x] = src->data[sy * src->w + sx];
        }
    }

    return 0;
}

static uint8_t lbp_code_at(const uint8_t *p, int stride)
{
    uint8_t c = p[0];
    uint8_t code = 0;

    code |= (uint8_t)((p[-stride - 1] >= c) << 7);
    code |= (uint8_t)((p[-stride] >= c) << 6);
    code |= (uint8_t)((p[-stride + 1] >= c) << 5);
    code |= (uint8_t)((p[1] >= c) << 4);
    code |= (uint8_t)((p[stride + 1] >= c) << 3);
    code |= (uint8_t)((p[stride] >= c) << 2);
    code |= (uint8_t)((p[stride - 1] >= c) << 1);
    code |= (uint8_t)((p[-1] >= c) << 0);

    return code;
}

static void extract_lbph(const GrayImage *img, float *feat, uint32_t dim)
{
    int x;
    int y;
    int gx;
    int gy;
    uint32_t i;
    float eps = 1e-6f;

    if (dim != FL_DIM)
    {
        return;
    }

    memset(feat, 0, sizeof(float) * dim);

    for (y = 1; y < img->h - 1; ++y)
    {
        const uint8_t *row = img->data + y * img->w;
        gy = (y * FL_GRID_Y) / img->h;
        if (gy >= FL_GRID_Y)
        {
            gy = FL_GRID_Y - 1;
        }

        for (x = 1; x < img->w - 1; ++x)
        {
            uint8_t code = lbp_code_at(row + x, img->w);
            gx = (x * FL_GRID_X) / img->w;
            if (gx >= FL_GRID_X)
            {
                gx = FL_GRID_X - 1;
            }
            feat[((gy * FL_GRID_X + gx) * FL_HIST_BINS) + code] += 1.0f;
        }
    }

    for (i = 0; i < (uint32_t)(FL_GRID_X * FL_GRID_Y); ++i)
    {
        uint32_t j;
        float sum = 0.0f;
        float *cell = feat + i * FL_HIST_BINS;

        for (j = 0; j < FL_HIST_BINS; ++j)
        {
            sum += cell[j];
        }
        sum += eps;
        for (j = 0; j < FL_HIST_BINS; ++j)
        {
            cell[j] /= sum;
        }
    }
}

static int build_feature_from_image(const char *path, float *feat, uint32_t dim)
{
    GrayImage src;
    GrayImage norm;
    int ret;

    memset(&src, 0, sizeof(src));
    memset(&norm, 0, sizeof(norm));

    ret = load_pgm(path, &src);
    if (ret < 0)
    {
        return -1;
    }

    ret = resize_nn(&src, &norm, FL_FACE_W, FL_FACE_H);
    free_image(&src);
    if (ret < 0)
    {
        fprintf(stderr, "resize failed for %s\n", path);
        return -1;
    }

    hist_equalize(norm.data, norm.w, norm.h);
    extract_lbph(&norm, feat, dim);
    free_image(&norm);
    return 0;
}

static float chi_square_dist(const float *a, const float *b, uint32_t dim)
{
    uint32_t i;
    float d = 0.0f;
    float eps = 1e-8f;

    for (i = 0; i < dim; ++i)
    {
        float s = a[i] + b[i] + eps;
        float t = a[i] - b[i];
        d += (t * t) / s;
    }

    return d;
}

static void db_free(FaceDb *db)
{
    uint32_t i;

    if (!db)
    {
        return;
    }

    for (i = 0; i < db->count; ++i)
    {
        free(db->records[i].feat);
        db->records[i].feat = NULL;
    }
    free(db->records);
    db->records = NULL;
    db->count = 0;
    db->dim = 0;
}

static int db_load(const char *path, FaceDb *db)
{
    FILE *fp;
    char magic[8];
    uint32_t version;
    uint32_t i;

    memset(db, 0, sizeof(*db));

    fp = fopen(path, "rb");
    if (!fp)
    {
        if (errno == ENOENT)
        {
            db->dim = FL_DIM;
            return 0;
        }
        fprintf(stderr, "open db %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic) ||
        memcmp(magic, FL_MAGIC, sizeof(magic)) != 0)
    {
        fprintf(stderr, "invalid db magic\n");
        fclose(fp);
        return -1;
    }

    if (fread(&version, sizeof(version), 1, fp) != 1 ||
        fread(&db->dim, sizeof(db->dim), 1, fp) != 1 ||
        fread(&db->count, sizeof(db->count), 1, fp) != 1)
    {
        fprintf(stderr, "read db header failed\n");
        fclose(fp);
        return -1;
    }

    if (version != FL_VERSION || db->dim != FL_DIM)
    {
        fprintf(stderr, "db format mismatch\n");
        fclose(fp);
        return -1;
    }

    if (db->count == 0)
    {
        fclose(fp);
        return 0;
    }

    db->records = (DbRecord *)calloc(db->count, sizeof(DbRecord));
    if (!db->records)
    {
        fclose(fp);
        return -1;
    }

    for (i = 0; i < db->count; ++i)
    {
        db->records[i].feat = (float *)malloc(sizeof(float) * db->dim);
        if (!db->records[i].feat)
        {
            db_free(db);
            fclose(fp);
            return -1;
        }

        if (fread(db->records[i].label, 1, FL_LABEL_MAX, fp) != FL_LABEL_MAX ||
            fread(db->records[i].feat, sizeof(float), db->dim, fp) != db->dim)
        {
            fprintf(stderr, "read db record failed\n");
            db_free(db);
            fclose(fp);
            return -1;
        }
        db->records[i].label[FL_LABEL_MAX - 1] = '\0';
    }

    fclose(fp);
    return 0;
}

static int db_save(const char *path, const FaceDb *db)
{
    FILE *fp;
    uint32_t version = FL_VERSION;
    uint32_t i;

    fp = fopen(path, "wb");
    if (!fp)
    {
        fprintf(stderr, "open db %s for write failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (fwrite(FL_MAGIC, 1, 8, fp) != 8 ||
        fwrite(&version, sizeof(version), 1, fp) != 1 ||
        fwrite(&db->dim, sizeof(db->dim), 1, fp) != 1 ||
        fwrite(&db->count, sizeof(db->count), 1, fp) != 1)
    {
        fclose(fp);
        return -1;
    }

    for (i = 0; i < db->count; ++i)
    {
        char label[FL_LABEL_MAX] = {0};
        strncpy(label, db->records[i].label, FL_LABEL_MAX - 1);

        if (fwrite(label, 1, FL_LABEL_MAX, fp) != FL_LABEL_MAX ||
            fwrite(db->records[i].feat, sizeof(float), db->dim, fp) != db->dim)
        {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    return 0;
}

static int db_find_label(const FaceDb *db, const char *label)
{
    uint32_t i;
    for (i = 0; i < db->count; ++i)
    {
        if (strncmp(db->records[i].label, label, FL_LABEL_MAX) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static int db_upsert(FaceDb *db, const char *label, const float *feat)
{
    int idx = db_find_label(db, label);

    if (idx >= 0)
    {
        memcpy(db->records[idx].feat, feat, sizeof(float) * db->dim);
        return 0;
    }

    {
        DbRecord *nrecs = (DbRecord *)realloc(db->records, sizeof(DbRecord) * (db->count + 1));
        if (!nrecs)
        {
            return -1;
        }
        db->records = nrecs;
    }

    db->records[db->count].feat = (float *)malloc(sizeof(float) * db->dim);
    if (!db->records[db->count].feat)
    {
        return -1;
    }

    strncpy(db->records[db->count].label, label, FL_LABEL_MAX - 1);
    db->records[db->count].label[FL_LABEL_MAX - 1] = '\0';
    memcpy(db->records[db->count].feat, feat, sizeof(float) * db->dim);
    db->count++;
    return 0;
}

static int cmd_enroll(int argc, char **argv)
{
    const char *db_path;
    const char *label;
    FaceDb db;
    float *mean;
    float *tmp;
    int i;
    int n;

    if (argc < 6)
    {
        return -1;
    }

    db_path = argv[2];
    label = argv[3];
    n = argc - 4;

    if (strlen(label) >= FL_LABEL_MAX)
    {
        fprintf(stderr, "label too long (max %d)\n", FL_LABEL_MAX - 1);
        return -1;
    }

    mean = (float *)calloc(FL_DIM, sizeof(float));
    tmp = (float *)malloc(sizeof(float) * FL_DIM);
    if (!mean || !tmp)
    {
        free(mean);
        free(tmp);
        return -1;
    }

    for (i = 4; i < argc; ++i)
    {
        uint32_t j;
        if (build_feature_from_image(argv[i], tmp, FL_DIM) < 0)
        {
            free(mean);
            free(tmp);
            return -1;
        }
        for (j = 0; j < FL_DIM; ++j)
        {
            mean[j] += tmp[j];
        }
    }

    {
        uint32_t j;
        for (j = 0; j < FL_DIM; ++j)
        {
            mean[j] /= (float)n;
        }
    }

    if (db_load(db_path, &db) < 0)
    {
        free(mean);
        free(tmp);
        return -1;
    }

    if (db_upsert(&db, label, mean) < 0)
    {
        fprintf(stderr, "db upsert failed\n");
        db_free(&db);
        free(mean);
        free(tmp);
        return -1;
    }

    if (db_save(db_path, &db) < 0)
    {
        fprintf(stderr, "db save failed\n");
        db_free(&db);
        free(mean);
        free(tmp);
        return -1;
    }

    printf("enrolled label=%s images=%d db_count=%u\n", label, n, db.count);

    db_free(&db);
    free(mean);
    free(tmp);
    return 0;
}

static int find_best(const FaceDb *db, const float *feat, int *best_idx, float *best_dist)
{
    uint32_t i;
    float bd = 1e30f;
    int bi = -1;

    if (db->count == 0)
    {
        return -1;
    }

    for (i = 0; i < db->count; ++i)
    {
        float d = chi_square_dist(db->records[i].feat, feat, db->dim);
        if (d < bd)
        {
            bd = d;
            bi = (int)i;
        }
    }

    *best_idx = bi;
    *best_dist = bd;
    return 0;
}

static int cmd_predict(int argc, char **argv)
{
    const char *db_path;
    const char *img_path;
    float threshold = -1.0f;
    FaceDb db;
    float *feat;
    int best_idx;
    float best_dist;

    if (argc < 4)
    {
        return -1;
    }

    db_path = argv[2];
    img_path = argv[3];
    if (argc >= 5)
    {
        threshold = (float)atof(argv[4]);
    }

    if (db_load(db_path, &db) < 0)
    {
        return -1;
    }

    feat = (float *)malloc(sizeof(float) * FL_DIM);
    if (!feat)
    {
        db_free(&db);
        return -1;
    }

    if (build_feature_from_image(img_path, feat, FL_DIM) < 0)
    {
        free(feat);
        db_free(&db);
        return -1;
    }

    if (find_best(&db, feat, &best_idx, &best_dist) < 0)
    {
        fprintf(stderr, "db is empty\n");
        free(feat);
        db_free(&db);
        return -1;
    }

    if (threshold > 0.0f && best_dist > threshold)
    {
        printf("predict: unknown (best_dist=%.6f threshold=%.6f)\n", best_dist, threshold);
    }
    else
    {
        printf("predict: %s (dist=%.6f)\n", db.records[best_idx].label, best_dist);
    }

    free(feat);
    db_free(&db);
    return 0;
}

static int cmd_verify(int argc, char **argv)
{
    const char *db_path;
    const char *label;
    const char *img_path;
    float threshold = 1.5f;
    FaceDb db;
    float *feat;
    int idx;
    float dist;

    if (argc < 5)
    {
        return -1;
    }

    db_path = argv[2];
    label = argv[3];
    img_path = argv[4];
    if (argc >= 6)
    {
        threshold = (float)atof(argv[5]);
    }

    if (db_load(db_path, &db) < 0)
    {
        return -1;
    }

    idx = db_find_label(&db, label);
    if (idx < 0)
    {
        fprintf(stderr, "label not found: %s\n", label);
        db_free(&db);
        return -1;
    }

    feat = (float *)malloc(sizeof(float) * FL_DIM);
    if (!feat)
    {
        db_free(&db);
        return -1;
    }

    if (build_feature_from_image(img_path, feat, FL_DIM) < 0)
    {
        free(feat);
        db_free(&db);
        return -1;
    }

    dist = chi_square_dist(db.records[idx].feat, feat, db.dim);
    printf("verify: %s dist=%.6f threshold=%.6f => %s\n",
           label, dist, threshold, (dist <= threshold) ? "PASS" : "FAIL");

    free(feat);
    db_free(&db);
    return 0;
}

static int cmd_list(int argc, char **argv)
{
    FaceDb db;
    uint32_t i;

    if (argc < 3)
    {
        return -1;
    }

    if (db_load(argv[2], &db) < 0)
    {
        return -1;
    }

    printf("db: dim=%u count=%u\n", db.dim, db.count);
    for (i = 0; i < db.count; ++i)
    {
        printf("  [%u] %s\n", i, db.records[i].label);
    }

    db_free(&db);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "enroll") == 0)
    {
        if (cmd_enroll(argc, argv) < 0)
        {
            usage(argv[0]);
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "predict") == 0)
    {
        if (cmd_predict(argc, argv) < 0)
        {
            usage(argv[0]);
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "verify") == 0)
    {
        if (cmd_verify(argc, argv) < 0)
        {
            usage(argv[0]);
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "list") == 0)
    {
        if (cmd_list(argc, argv) < 0)
        {
            usage(argv[0]);
            return 1;
        }
        return 0;
    }

    usage(argv[0]);
    return 1;
}
