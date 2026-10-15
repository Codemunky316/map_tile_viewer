#include <curl/curl.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include "util.h"

// https://tile.openstreetmap.org/{z}/{x}/{y}.png
#define TILES_URL_TEMPLATE "https://tile.openstreetmap.org/{%d}/{%d}/{%d}.png"

#define WINDOW_WIDTH 1024
#define WINDOW_HEIGHT 768

// #define MAX_ZOOM_LEVEL 20
// TODO(mike): work up to 10, then 20. but 10 would already be 1024 x 1024 total tiles.
// we need to first work on calculating the appropriate tiles for the viewport
#define MAX_ZOOM_LEVEL 4

#define MIN_ZOOM_LEVEL 0


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// Asset Allocator
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// TODO(mike): combine two (potentially more) Slab allocators,
/// each with their fixed slab size but one for small slabs, one for big slabs.
/// each with their own free lists (TODO)
/// each growable with realloc. this means direct pointers are NOT STABLE AND OUGHT NOT TO BE USED
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// TODO(mike): slab sizes will likely need rightsizing
#define SMALL_SLAB_SIZE 4096 // 4KiB
#define LARGE_SLAB_SIZE 20480 // 20KiB

// TODO(mike): free list
typedef struct {
    char *memory;
    const size_t slab_size;
    size_t slab_offset;
    size_t slab_capacity;
} SlabAllocator;

typedef struct {
    // TODO(mike): maybe variable number of allocators? No, not VLAs. VLAs are bad.
    SlabAllocator allocators[2];
} AssetAllocator;

typedef struct {
    const size_t idx_allocator;
    const size_t offset;
    const size_t size;
} AssetAllocation;

// Note(mike): might be useful for TextureAtlas, might be wasted effort, we'll see
typedef struct {
    size_t *idx_allocators;
    size_t *offsets;
    size_t *sizes;
    size_t len;
    size_t cap;
} AssetAllocationSOA;

AssetAllocator
aa_create_asset_allocator() {
    // TODO(mike): allocation error handling; ignore for now

    // small slab allocator
    size_t small_slab_capacity = 100;
    char *small_slab_memory = malloc(SMALL_SLAB_SIZE * small_slab_capacity);
    memset(small_slab_memory, 0, SMALL_SLAB_SIZE * small_slab_capacity);

    // large slab allocator
    size_t large_slab_capacity = 100;
    char *large_slab_memory = malloc(LARGE_SLAB_SIZE * large_slab_capacity);
    memset(large_slab_memory, 0, LARGE_SLAB_SIZE * large_slab_capacity);

    return (AssetAllocator) {
        .allocators = {
        {
            .memory = small_slab_memory,
            .slab_size = SMALL_SLAB_SIZE,
            .slab_offset = 0,
            .slab_capacity = small_slab_capacity,
            },
        {
            .memory = large_slab_memory,
            .slab_size = LARGE_SLAB_SIZE,
            .slab_offset = 0,
            .slab_capacity = large_slab_capacity,
            }
        }
    };
}

void
aa_destroy_asset_allocator(AssetAllocator *allocator) {
    allocator->allocators[0].slab_capacity = 0;
    allocator->allocators[0].slab_offset = 0;
    free(allocator->allocators[0].memory);
    allocator->allocators[0].memory = NULL;

    allocator->allocators[1].slab_capacity = 0;
    allocator->allocators[1].slab_offset = 0;
    free(allocator->allocators[1].memory);
    allocator->allocators[1].memory = NULL;
}

AssetAllocation
aa_allocate_asset(AssetAllocator *allocator, const size_t size) {
    // TODO(mike): need to check alignment?

    // we allocate up to 4 slabs in the small one, otherwise the large one
    // TODO(mike): this will likely get fine-tuned later
    int idx_allocator;
    if (size / SMALL_SLAB_SIZE <= 4) {
        idx_allocator = 0;
    } else {
        idx_allocator = 1;
    }

    // TODO(mike): we'll need a free list per allocator once we want to unload some images

    // TODO(mike): bit more fine-grained care maybe
    const size_t number_of_slabs = size / SMALL_SLAB_SIZE + 1;

    int need_realloc = 0;
    while ((allocator->allocators[idx_allocator].slab_offset + number_of_slabs)
            >= allocator->allocators[idx_allocator].slab_capacity) {
        need_realloc = 1;
        // TODO(mike): doubling might be a tad aggressive
        allocator->allocators[idx_allocator].slab_capacity *= 2;
    }

    if (need_realloc) {
        char *new_memory = realloc(
            allocator->allocators[idx_allocator].memory,
            allocator->allocators[idx_allocator].slab_capacity * allocator->allocators[idx_allocator].slab_size);
       if (new_memory == NULL) {
           // TODO(mike): better error handling!
           fprintf(stderr, "realloc() failed\n");
           exit(-1);
       }
        allocator->allocators[idx_allocator].memory = new_memory;
    }

    const size_t offset = allocator->allocators[idx_allocator].slab_offset;

    allocator->allocators[idx_allocator].slab_offset += number_of_slabs;

    return (AssetAllocation) {
        .idx_allocator =  idx_allocator,
        .offset = offset,
        .size = size,
    };
}




////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// main program
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int main(void) {

    CURL *curl_handle;
    CURLcode res;

    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_handle = curl_easy_init();
    if (!curl_handle) {
        fprintf(stderr, "curl_easy_init() failed\n");
        // lol, "leaked memory" allocated. my brother in christ, you exit the program
        return 1;
    }

    // TODO(mike): we'll need to use async stuff later but for lower zoom levels blocking will be fine
    curl_easy_setopt(curl_handle, CURLOPT_URL, "https://tile.openstreetmap.org/1/0/0.png");
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    res = curl_easy_perform(curl_handle);
    if(res != CURLE_OK) {
        fprintf(stderr, "error: %s\n", curl_easy_strerror(res));
        return 1;
    }

    struct MemoryStruct chunk2;
    chunk2.memory = malloc(1);
    chunk2.size = 0;
    curl_easy_setopt(curl_handle, CURLOPT_URL, "https://tile.openstreetmap.org/1/0/1.png");
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk2);
    res = curl_easy_perform(curl_handle);
    if(res != CURLE_OK) {
        fprintf(stderr, "error: %s\n", curl_easy_strerror(res));
        return 1;
    }

    struct MemoryStruct chunk3;
    chunk3.memory = malloc(1);
    chunk3.size = 0;
    curl_easy_setopt(curl_handle, CURLOPT_URL, "https://tile.openstreetmap.org/1/1/0.png");
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk3);
    res = curl_easy_perform(curl_handle);
    if(res != CURLE_OK) {
        fprintf(stderr, "error: %s\n", curl_easy_strerror(res));
        return 1;
    }

    struct MemoryStruct chunk4;
    chunk4.memory = malloc(1);
    chunk4.size = 0;
    curl_easy_setopt(curl_handle, CURLOPT_URL, "https://tile.openstreetmap.org/1/1/1.png");
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk4);
    res = curl_easy_perform(curl_handle);
    if(res != CURLE_OK) {
        fprintf(stderr, "error: %s\n", curl_easy_strerror(res));
        return 1;
    }



    int total_tiles_per_axis;

    int x_tiles_per_axis;
    int y_tiles_per_axis;

    // NOTE(mike): we render the hard-coded zoom level first, then we go in
    int zoom_level = 2;

    int x = 0;
    int y = 0;

    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Mappy McMapface");
    SetTargetFPS(60);

    // TODO(mike): do it naively first, loading texture for each tile
    Image img = LoadImageFromMemory(".png", (const unsigned char*) chunk.memory, (int) chunk.size);
    Texture2D texture = LoadTextureFromImage(img);
    Image img2 = LoadImageFromMemory(".png", (const unsigned char*) chunk2.memory, (int) chunk2.size);
    Texture2D texture2 = LoadTextureFromImage(img2);
    Image img3 = LoadImageFromMemory(".png", (const unsigned char*) chunk3.memory, (int) chunk3.size);
    Texture2D texture3 = LoadTextureFromImage(img3);
    Image img4 = LoadImageFromMemory(".png", (const unsigned char*) chunk4.memory, (int) chunk4.size);
    Texture2D texture4 = LoadTextureFromImage(img4);

    while (!WindowShouldClose()) {
        // TODO(mike): read input i.e. mouse movements, scrolling

        total_tiles_per_axis = 2 << zoom_level;


        // TODO(mike): display the entire world first. later we will work on appropriate tiles per zoom_level for the viewport

        BeginDrawing();
        ClearBackground(RAYWHITE);

        // Draw texture centered on screen
        // DrawTexture(texture,
        //     GetScreenWidth() / 2 - texture.width / 2,
        //     GetScreenHeight() / 2 - texture.height / 2,
        //     WHITE);

        DrawTexture(texture,
            0,0,
            WHITE);
        DrawTexture(texture2,
            0,img2.height,
            WHITE);
        DrawTexture(texture3,
            img3.width,0,
            WHITE);
        DrawTexture(texture4,
            img3.width,img.height,
            WHITE);

        EndDrawing();
    }

    curl_easy_cleanup(curl_handle);

    return 0;
}
