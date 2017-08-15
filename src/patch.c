/*
The MIT License (MIT)

Copyright (c) 2015 chenqi

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <libgen.h>

#include "patch.h"
#include "util.h"
#include "log.h"
#include "http.h"

static CRScode Patch_match(const char *srcFilename, const char *dstFilename,
                           const fileDigest_t *fd, const diffResult_t *dr) {
    LOGI("begin\n");
    if(!srcFilename || !dstFilename || !fd || !dr) {
        LOGE("end %d\n", CRS_PARAM_ERROR);
        return CRS_PARAM_ERROR;
    }

    FILE *f1 = fopen(srcFilename, "rb");
    if(!f1){
        LOGE("source file fopen error %s\n", strerror(errno));
        return CRS_FILE_ERROR;
    }

    FILE *f2 = fopen(dstFilename, "rb+");
    if(!f2){
        LOGE("dest file fopen error %s\n", strerror(errno));
        fclose(f1);
        return CRS_FILE_ERROR;
    }

    CRScode code = CRS_OK;
    uint8_t *buf = malloc(fd->blockSize);

    for(int i=0; i<dr->totalNum; ++i) {
        if(dr->offsets[i] >= 0) {
            fseek(f1, dr->offsets[i], SEEK_SET);
            fread(buf, 1, fd->blockSize, f1);

            fseek(f2, i*fd->blockSize, SEEK_SET);
            fwrite(buf, 1, fd->blockSize, f2);
        }
    }

    size_t restSize = fd->fileSize % fd->blockSize;
    if(restSize > 0){

        fseek(f2, -restSize, SEEK_END);
        fwrite(fd->restData, 1, restSize, f2);
    }

    free(buf);
    fclose(f1);
    fclose(f2);
    LOGI("end %d\n", code);
    return code;
}

//continuous blocks, used for reduce http range frequency
typedef struct combineblock_t {
    size_t pos; //block start position
    size_t got; //data fwrite size, used for HTTP retry
    size_t len; //block length
} combineblock_t;

static uint32_t Patch_missCombine(const diffResult_t *dr, combineblock_t *cb, uint32_t blockSize) {
    uint32_t combineNum = 0;
    int missNum = dr->totalNum - dr->matchNum - dr->cacheNum;
    if(missNum == 0) {
        LOGW("missNum is Zero, maybe cache done!\n");
        return combineNum;
    }
    if(missNum < 0) {
        LOGE("WTF: matchNum %d bigger than totalNum %d\n", dr->matchNum, dr->totalNum);
        return combineNum;
    }
    int i;
    uint32_t j;
    for(i=0; i<dr->totalNum; ++i) {
        if(dr->offsets[i] == -1) {
            for(j=0; j < combineNum; ++j) {
                if(cb[j].pos + cb[j].len == i * blockSize) {
                    cb[j].len += blockSize;
                    break;
                }
            }
            if(j == combineNum) {
                cb[combineNum].pos = i*blockSize;
                cb[combineNum].len = blockSize;
                ++combineNum;
            }
        }
    }
    LOGI("combineblocks Num = %d\n", combineNum);
    return combineNum;
}

typedef struct rangedata_t {
    combineblock_t *cb; //ref to one Patch_miss()
    FILE *file; //ref to one Patch_miss()
    char *basename; //ref to one Patch_miss()
    size_t cacheBytes;
} rangedata_t;

static size_t Range_callback(void *data, size_t size, size_t nmemb, void *userp) {
    rangedata_t *rd = (rangedata_t*)userp;
    size_t realSize = size * nmemb;
    fseek(rd->file, rd->cb->pos + rd->cb->got, SEEK_SET);
    fwrite(data, size, nmemb, rd->file);
    rd->cb->got += realSize;
    rd->cacheBytes += realSize;
    int isCancel = crs_callback_patch(rd->basename, rd->cb->got, 0, 0);
    return (isCancel == 0) ? realSize : 0;
}

static CRScode Patch_miss(const char *srcFilename, const char *dstFilename, const char *url,
                          const fileDigest_t *fd, const diffResult_t *dr) {
    LOGI("begin\n");
    if(!srcFilename || !dstFilename || !fd || !dr) {
        LOGE("end %d\n", CRS_PARAM_ERROR);
        return CRS_PARAM_ERROR;
    }

    int missNum = dr->totalNum - dr->matchNum - dr->cacheNum;
    if(missNum == 0) {
        LOGI("end missblocks = 0\n");
        return CRS_OK;
    }

    FILE *f = fopen(dstFilename, "rb+");
    if(!f){
        LOGE("%s\n", dstFilename);
        LOGE("end fopen error %s\n", strerror(errno));
        return CRS_FILE_ERROR;
    }

    //combine continuous blocks, no more than missNum
    combineblock_t *cb = calloc(missNum, sizeof(combineblock_t) );
    uint32_t cbNum = Patch_missCombine(dr, cb, fd->blockSize);

    CRScode code = CRS_OK;
    rangedata_t rd;
    rd.file = f;
    rd.cacheBytes = fd->fileSize - (missNum * fd->blockSize);

    //Some compiler maybe change basename() param
    //Do not free(basename) since it maybe inside of fullname
    char *tempname = strdup(srcFilename);
    rd.basename = basename(tempname);
    CURL *curl = curl_easy_init();
    for(uint32_t i=0; i< cbNum; ++i) {
        rd.cb = &cb[i];
        char range[32];
        long rangeFrom;
        long rangeTo;

        int retry = 10;
        while(retry-- > 0) {
            rangeFrom = cb[i].pos + cb[i].got;
            rangeTo = cb[i].pos + cb[i].len - 1;
            snprintf(range, 32, "%ld-%ld", rangeFrom, rangeTo);

            if(rangeFrom >= rangeTo) {
                LOGE("range request %ld-%ld wrong\n", rangeFrom, rangeTo);
                LOGE("This should not happend! But continue NextBlock\n");
                code = CRS_OK;
                break;
            }

            CURLcode curlcode = HTTP_Range(curl, url, range, (void*)Range_callback, &rd);

            switch(curlcode) {
            case CURLE_OK:
                code = CRS_OK;
                break;
            case CURLE_HTTP_RETURNED_ERROR:
                LOGE("HTTP request/response header wrong!\n");
                retry = -1;
                code = CRS_HTTP_ERROR;
                break;
            case CURLE_OPERATION_TIMEDOUT: //timeout
                retry++;
                code = CRS_HTTP_ERROR;
                break;
            default:
                code = CRS_HTTP_ERROR;
                break;
            }

            if(CRS_OK == code) { //got it
                break;
            } else {
                LOGE("curl code %d\n", curlcode);
            }
        }//end of while

    }//end of for

    curl_easy_cleanup(curl);
    free(tempname);
    free(cb);
    fclose(f);
    LOGI("end %d\n", code);
    return code;
}

CRScode Patch_perform(const char *srcFilename, const char *dstFilename, const char *url,
                      const fileDigest_t *fd, const diffResult_t *dr) {
    LOGI("begin\n");

    if(!srcFilename || !dstFilename || !url || !fd || !dr) {
        LOGE("end %d\n", CRS_PARAM_ERROR);
        return CRS_PARAM_ERROR;
    }

    CRScode code = CRS_OK;
    do {
        if(0 != access(srcFilename, F_OK)) {
            LOGE("src file not exist %s\n", strerror(errno));
            LOGE("%s\n", srcFilename);
            code = CRS_FILE_ERROR;
            break;
        }
        if(0 != access(dstFilename, F_OK)) {
            FILE *f = fopen(dstFilename, "ab+");
            if(f) {
                fclose(f);
            } else {
                LOGE("dst file create fail %s\n", strerror(errno));
                LOGE("%s\n", dstFilename);
                code = CRS_FILE_ERROR;
                break;
            }
        }
        struct stat st;
        if(stat(dstFilename, &st) != 0) {
            LOGE("dst file stat fail %s\n", strerror(errno));
            LOGE("%s\n", dstFilename);
            code = CRS_FILE_ERROR;
            break;
        }
        if((size_t)st.st_size != fd->fileSize) {
            if(0 != truncate(dstFilename, fd->fileSize)) {
                LOGE("dest file truncate %dBytes error %s\n", fd->fileSize, strerror(errno));
                code = CRS_FILE_ERROR;
                break;
            }
        }

        //Patch_match Blocks
        code = Patch_match(srcFilename, dstFilename, fd, dr);
        if(code != CRS_OK) break;

        //Patch_miss Blocks
        code = Patch_miss(srcFilename, dstFilename, url, fd, dr);
        if(code != CRS_OK) break;

        char *tempname = strdup(srcFilename);
        char *name = basename(tempname);
        crs_callback_patch(name, fd->fileSize, 0, 1);
        free(tempname);

        uint8_t hash[CRS_STRONG_DIGEST_SIZE];
        Digest_CalcStrong_File(dstFilename, hash);
        char * hashString = Util_hex_string(hash, CRS_STRONG_DIGEST_SIZE);
        LOGI("fileDigest = %s\n", hashString);
        free(hashString);
        code = (0 == memcmp(hash, fd->fileDigest, CRS_STRONG_DIGEST_SIZE)) ? CRS_OK : CRS_BUG ;

    } while (0);

    LOGI("end %d\n", code);
    return code;
}
