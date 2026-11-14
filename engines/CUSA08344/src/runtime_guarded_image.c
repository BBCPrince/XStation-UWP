/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime_guarded_image.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Binds first-touch commitment to this title/version's tested guard image.
 * Provenance: prepared-redzone-01/provenance.json. This is not a claim that
 * every guest function has a guard; uncovered sites stay in the inventory. */
int runtime_guarded_image_verify(const char *path) {
    static const unsigned char expected[32]={
        0x78,0x3f,0x69,0x55,0xf1,0x38,0x31,0xae,0x0f,0x04,0xf3,0x50,0xdb,0xed,0x99,0xd7,
        0x82,0x23,0x9e,0x91,0x30,0x9e,0xfc,0xb3,0xa5,0x77,0xc0,0x57,0x37,0xcd,0xc4,0x39};
    /* God of War port (2026-11-04): God of War (CUSA07408 v1.35) out/boot-linked.bin, prepared
     * with the same red-zone guards (NOTES.md: 110 sites left uncovered), the image of every PC
     * run with the Xbox memory backend. */
    static const unsigned char expected_gow[32]={
        0x5b,0x56,0x22,0xf7,0x8e,0x55,0x1a,0x88,0xc4,0x8b,0x23,0x67,0xe8,0x1a,0x98,0xed,
        0xbc,0x0d,0xe7,0x47,0x8c,0x0a,0xc9,0x59,0xc1,0xba,0x97,0x16,0x0d,0xcc,0xd6,0x25};
    /* Spider-Man port (2026-11-05): Marvel's Spider-Man (CUSA11994 v1.19) out/boot-linked.bin, with the
     * same red-zone guards (jump tables and SSE4a included), the image of every PC run. */
    static const unsigned char expected_sm[32]={
        0x44, 0xe4, 0x05, 0x05, 0x13, 0xe7, 0x3b, 0xb9, 0x2d, 0x6f, 0x7b, 0xfd, 0x6c, 0xc0, 0x20, 0xbb,
        0xe6, 0xe7, 0xe6, 0x94, 0xa6, 0x5c, 0x63, 0x68, 0xc6, 0xd4, 0xe1, 0x28, 0x0e, 0xd8, 0x16, 0x64};
    /* Detroit port (2026-11-08): Detroit: Become Human (CUSA08344 v1.00) out/boot-linked.bin, the image of
     * every PC run (red-zone guards as Spider-Man's). */
    static const unsigned char expected_dt[32]={
        0x23,0xe1,0xb2,0xe1,0xce,0xb2,0x2d,0xa7,0x30,0xc2,0x3f,0x77,0x57,0x87,0x2a,0x01,
        0xf7,0xb8,0x91,0x3f,0x68,0xaa,0x38,0x63,0x52,0xfa,0x7b,0x77,0x10,0x34,0x64,0x19};
    if (!path) return 0;
    FILE *file=fopen(path,"rb");
    if (!file) return 0;
    BCRYPT_ALG_HANDLE algorithm=NULL;
    BCRYPT_HASH_HANDLE hash=NULL;
    unsigned char *object=NULL, digest[32], data[16384];
    DWORD object_bytes=0,returned=0;
    int ok=0;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)<0 ||
        BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,(PUCHAR)&object_bytes,sizeof(object_bytes),&returned,0)<0)
        goto finish;
    object=malloc(object_bytes);
    if (!object || BCryptCreateHash(algorithm,&hash,object,object_bytes,NULL,0,0)<0) goto finish;
    size_t count;
    while ((count=fread(data,1,sizeof(data),file))!=0)
        if (BCryptHashData(hash,data,(ULONG)count,0)<0) goto finish;
    if (ferror(file) || BCryptFinishHash(hash,digest,sizeof(digest),0)<0) goto finish;
    ok=!memcmp(digest,expected,sizeof(digest)) || !memcmp(digest,expected_gow,sizeof(digest)) ||
       !memcmp(digest,expected_sm,sizeof(digest)) || !memcmp(digest,expected_dt,sizeof(digest));
finish:
    if (hash) BCryptDestroyHash(hash);
    free(object);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    fclose(file);
    return ok;
}
