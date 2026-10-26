#include <curl/curl.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

// https://tile.openstreetmap.org/{z}/{x}/{y}.png
// #define TILES_URL_TEMPLATE "https://tile.openstreetmap.org/%d/%d/%d.png"
#define TILES_URL_TEMPLATE "http://localhost:8080/%d/%d/%d.png"

#define WINDOW_WIDTH 1024
#define WINDOW_HEIGHT 768

// #define MAX_ZOOM_LEVEL 20
// TODO(mike): work up to 10, then 20. but 10 would already be 1024 x 1024 total tiles.
// we need to first work on calculating the appropriate tiles for the viewport
#define MAX_ZOOM_LEVEL 4

#define MIN_ZOOM_LEVEL 0

// Note(mike): OpenStreetMap Raster tiles are 256 x 256 pixels
#define TILE_PIXELS_X 256
#define TILE_PIXELS_Y 256

#define TILES_X 10
#define TILES_Y 10

// Note(mike): we assume 4 bytes TODO(mike): this might need to be flexible in the future
#define BYTES_PER_PIXEL 4

#define CURSOR_SPEED_X 100
#define CURSOR_SPEED_Y 100


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// GIS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef struct {
    int x_min;
    int x_max;
    int y_min;
    int y_max;
} TileRange;

TileRange
gis_visible_tiles(double center_x, double center_y, int viewport_width, int viewport_height) {
    double left   = center_x - viewport_width  / 2.0;
    double right  = center_x + viewport_width  / 2.0;
    double top    = center_y - viewport_height / 2.0;
    double bottom = center_y + viewport_height / 2.0;

    TileRange r = {
        .x_min = (int)floor(left / TILE_PIXELS_X),
        .x_max = (int)floor((right - 1.0) / TILE_PIXELS_X),
        .y_min = (int)floor(top / TILE_PIXELS_Y),
        .y_max = (int)floor((bottom - 1.0) / TILE_PIXELS_Y)
    };

    return r;

}


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
    size_t idx_allocator;
    size_t offset;
    size_t size;
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

    const size_t offset = allocator->allocators[idx_allocator].slab_offset * allocator->allocators[idx_allocator].slab_size;

    allocator->allocators[idx_allocator].slab_offset += number_of_slabs;

    return (AssetAllocation) {
        .idx_allocator =  idx_allocator,
        .offset = offset,
        .size = size,
    };
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// scratch buffer
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef struct {
    char *memory;
    size_t len;
    size_t cap;
} ScratchBuffer;

size_t
scratch_buffer_append(ScratchBuffer *buffer, void *content, size_t size) {
    int realloc_needed = 0;
    if (buffer->cap == 0) {
        realloc_needed = 1;
        // TODO(mike): maybe different size cap constant to not mix these two
        buffer->cap = SMALL_SLAB_SIZE;
    }
    while (buffer->len + size >= buffer->cap) {
        realloc_needed = 1;
        buffer->cap *= 2;
    }

    if (realloc_needed) {
        char *new_memory = realloc(buffer->memory, buffer->cap);
        if (new_memory == NULL) {
            fprintf(stderr, "realloc() failed\n");
            return -1;
        }
        buffer->memory = new_memory;
    }
    for (size_t i = 0; i < size; ++i) {
        buffer->memory[buffer->len++] = ((char*)content)[i];
    }
    // TODO(mike): i need to be sure about my API...
    return size;
}

void
scratch_buffer_reset(ScratchBuffer *buffer) {
    buffer->len = 0;
}

void
scratch_buffer_destroy(ScratchBuffer *buffer) {
    free(buffer->memory);
    buffer->memory = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// texture SOA
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef struct {
    size_t *idx_allocators;
    size_t *offsets;
    size_t *sizes;
    Image *images;
    Texture *textures;
    size_t len;
} TextureSOA;

TextureSOA
TextureSOA_create(const size_t len) {
    size_t *idx_allocators = calloc(1, sizeof(size_t) * len);
    size_t *offsets = calloc(1, sizeof(size_t) * len);
    size_t *sizes = calloc(1,sizeof(size_t) * len);
    Image *images = calloc(1,sizeof(Image) * len);
    Texture *textures = calloc(1,sizeof(Texture) * len);

    return (TextureSOA) {
    .idx_allocators = idx_allocators,
    .offsets = offsets,
    .sizes = sizes,
    .images = images,
    .textures = textures,
    .len = len,
    };
}

void
TextureSOA_destroy(TextureSOA *soa) {
    for (size_t i = 0; i < soa->len; ++i) {
        UnloadImage(soa->images[i]);
        UnloadTexture(soa->textures[i]);
    }
    free(soa->images);
    free(soa->textures);
    free(soa->idx_allocators);
    free(soa->offsets);
    free(soa->sizes);

    soa->images = NULL;
    soa->textures = NULL;
    soa->idx_allocators = NULL;
    soa->offsets = NULL;
    soa->sizes = NULL;
    soa->len = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// http client stuff
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///
// memory callback is called in chunks! i need a scratch arena for the memory
size_t
CURLWriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;

    ScratchBuffer *buffer = (ScratchBuffer *)userp;

    scratch_buffer_append(buffer, contents, realsize);

    return realsize;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// main program
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int main(void) {

    // main allocator
    AssetAllocator asset_allocator = aa_create_asset_allocator();

    CURL *curl_handle;
    CURLcode res;
    ScratchBuffer buffer = {};

    curl_handle = curl_easy_init();
    if (!curl_handle) {
        fprintf(stderr, "curl_easy_init() failed\n");
        // lol, "leaked memory" allocated. my brother in christ, you exit the program
        return 1;
    }

    double center_x = 0;
    double center_y = 0;

    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Mappy McMapface");
    SetTargetFPS(60);

    // try with framebuffer
    RenderTexture2D texture_buffer = LoadRenderTexture(TILE_PIXELS_X * TILES_X, TILE_PIXELS_Y * TILES_Y);

    // Note(mike): we draw to texture first, then we render the texture to the screen
    BeginTextureMode(texture_buffer);

    // TODO(mike): implement zoom level 10 for movement. limit it to a certain amount of tiles

    // TODO(mike): we'll need to use async stuff later but for lower zoom levels blocking will be fine
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, CURLWriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&buffer);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    // TODO(mike): load all ~million tiles for zoom level 10 just to test movement and rendering
    int zoom_level = 10;
    int total_tiles_per_axis = 1 << zoom_level;
    char url_buffer[1024] = {};

    TextureSOA texture_soa = TextureSOA_create(TILES_X * TILES_Y);

    size_t texture_idx = 0;
    int texture_y = 0;
    for (int y = 360; y < 370; ++y) {
        int texture_x = 0;
        for (int x = 535; x < 545; ++x) {
            //snprintf(url_buffer, sizeof url_buffer, "https://tile.openstreetmap.org/%d/%d/%d.png", zoom_level, x, y);
            snprintf(url_buffer, sizeof url_buffer, TILES_URL_TEMPLATE, zoom_level, x, y);
            curl_easy_setopt(curl_handle, CURLOPT_URL, url_buffer);

            res = curl_easy_perform(curl_handle);
            if(res != CURLE_OK) {
                fprintf(stderr, "error: %s\n", curl_easy_strerror(res));
                return 1;
            }

            AssetAllocation data = aa_allocate_asset(&asset_allocator, buffer.len);
            memcpy(asset_allocator.allocators[data.idx_allocator].memory + data.offset, buffer.memory, buffer.len);
            scratch_buffer_reset(&buffer);

            texture_soa.idx_allocators[texture_idx] = data.idx_allocator;
            texture_soa.offsets[texture_idx] = data.offset;
            texture_soa.sizes[texture_idx] = data.size;
            // TODO(mike): separate tile acquisition from loading textures, maybe?


            const unsigned char *memory = (const unsigned char *) asset_allocator.allocators[data.idx_allocator].memory + data.offset;
            texture_soa.images[texture_idx] = LoadImageFromMemory(".png", memory, (int) data.size);
            texture_soa.textures[texture_idx] = LoadTextureFromImage(texture_soa.images[texture_idx]);

            // DrawTexture(
            //     texture_soa.textures[texture_idx],
            //     texture_x * TILE_PIXELS_X,
            //     texture_y * TILE_PIXELS_Y,
            //     RAYWHITE
            //     );

            // TODO(mike): there's still a major bug here. X Tiles are repeating indefinitely and don't match to 10 tiles
            DrawTexturePro(
                texture_soa.textures[texture_idx],
                (Rectangle){
                    0, 0,
                    (float)TILE_PIXELS_X,
                    (float)-TILE_PIXELS_Y },
                (Rectangle){
                    texture_x * TILE_PIXELS_X,
                     TILE_PIXELS_Y * TILES_Y - texture_y * TILE_PIXELS_Y,
                    (float) TILE_PIXELS_X,
                    (float) TILE_PIXELS_Y
                },
                (Vector2){ 0, 0 },
            0,
                WHITE
            );



            texture_x++;
            texture_idx++;
        }
        texture_y++;
    }



    //
    // // TODO(mike): handle rotation
    // // top left
    // DrawTexture(texture,
    //     GetScreenWidth() / 2 - TILE_PIXELS_X,GetScreenHeight() / 2 - TILE_PIXELS_Y,
    //     WHITE);
    // // bottom left
    // DrawTexture(texture2,
    //     GetScreenWidth() / 2 - TILE_PIXELS_X,GetScreenHeight() / 2,
    //     WHITE);
    // // top right
    // DrawTexture(texture3,
    //     GetScreenWidth() / 2,GetScreenHeight() / 2 - TILE_PIXELS_Y,
    //     WHITE);
    // // bottom right
    // DrawTexture(texture4,
    // GetScreenWidth() / 2,GetScreenHeight() / 2,
    //     WHITE);

    EndTextureMode();

    while (!WindowShouldClose()) {
        // TODO(mike): read input i.e. mouse movements, scrolling, but first make it work with wasd and arrow keys

        float speed_x = 0;
        float speed_y = 0;
        float dt = GetFrameTime();

        ////////////////////////////////////////
        /// inputs
        ////////////////////////////////////////
        if (IsKeyDown(KEY_A)) {
            speed_x = -CURSOR_SPEED_X * dt;
        }
        if (IsKeyDown(KEY_D)) {
            speed_x = CURSOR_SPEED_X * dt;
        }
        if (IsKeyDown(KEY_W)) {
           speed_y = -CURSOR_SPEED_Y * dt;
        }
        if (IsKeyDown(KEY_S)) {
            speed_y = CURSOR_SPEED_Y * dt;
        }

        ////////////////////////////////////////
        /// update cursor position
        ////////////////////////////////////////
        center_x += speed_x;
        center_y += speed_y;

        // total_tiles_per_axis = 2 << zoom_level;

        ////////////////////////////////////////
        /// drawing
        ////////////////////////////////////////

        // Now we draw to the screen
        BeginDrawing();
        ClearBackground(RAYWHITE);

        // Draw texture centered on screen

        //DrawTexture(texture_buffer.texture,
        //GetScreenWidth() / 2 - texture.width / 2,
        //GetScreenHeight() / 2 - texture.height / 2,
        //WHITE
        //);

        // DrawTextureRec(
        //     texture_buffer.texture,
        //     (Rectangle) {
        //         GetScreenWidth() / 2 - TILE_PIXELS_X + center_x,
        //         GetScreenHeight() / 2 - TILE_PIXELS_Y + center_y,
        //         TILE_PIXELS_X,
        //         TILE_PIXELS_Y
        //     },
        //     (Vector2) {
        //         GetScreenWidth() / 2 - TILE_PIXELS_X /2,
        //         GetScreenHeight() / 2 - TILE_PIXELS_Y /2,
        //     },
        //     RAYWHITE
        //     );



        DrawTextureRec(
            texture_buffer.texture,
            (Rectangle) {
                4 * TILE_PIXELS_X + center_x,
                4 * TILE_PIXELS_Y + center_y,
                GetScreenWidth(),
                GetScreenHeight(),
            },
            (Vector2) {
                0,0
            },
            WHITE
            );


        // horizontal middle line
        DrawLine(
            0,
            GetScreenHeight() / 2,
            GetScreenWidth(),
            GetScreenHeight() /2,
            BLACK
            );

        // vertical middle line
        DrawLine(
            GetScreenWidth() / 2,
            0,
            GetScreenWidth() / 2,
            GetScreenHeight(),
            BLACK
            );

        // middle point
        DrawCircle(
            GetScreenWidth() / 2,
            GetScreenHeight() / 2,
            5.0f,
            BLACK
            );

        /// draw vertical and horizontal line around tiles

        // center to left
        for (int x = GetScreenWidth() / 2 - TILE_PIXELS_X; x > 0; x -= TILE_PIXELS_X) {
            DrawLine(
                x,
                0,
                x,
                GetScreenHeight(),
                BLACK
                );
        }
        // center to right
        for (int x = GetScreenWidth() / 2 + TILE_PIXELS_X; x < GetScreenWidth(); x += TILE_PIXELS_X) {
            DrawLine(
                x,
                0,
                x,
                GetScreenHeight(),
                BLACK
                );
        }

        // center up
        for (int y = GetScreenHeight() / 2 - TILE_PIXELS_Y; y > 0; y -= TILE_PIXELS_Y) {
            DrawLine(
                0,
                y,
                GetScreenWidth(),
                y,
                BLACK);
        }

        // center down
        for (int y = GetScreenHeight() / 2 + TILE_PIXELS_Y; y < GetScreenHeight(); y += TILE_PIXELS_Y) {
            DrawLine(
                0,
                y,
                GetScreenWidth(),
                y,
                BLACK);
        }


        EndDrawing();
    }

    curl_easy_cleanup(curl_handle);

    return 0;
}
