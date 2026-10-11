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
    curl_easy_setopt(curl_handle, CURLOPT_URL, "https://tile.openstreetmap.org/0/0/0.png");
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    res = curl_easy_perform(curl_handle);
    if(res != CURLE_OK) {
        fprintf(stderr, "error: %s\n", curl_easy_strerror(res));
    } else {
        printf("Size: %lu\n", (unsigned long)chunk.size);
        printf("Data: %s\n", chunk.memory);
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

    while (!WindowShouldClose()) {
        // TODO(mike): read input i.e. mouse movements, scrolling

        total_tiles_per_axis = 2 << zoom_level;


        // TODO(mike): display the entire world first. later we will work on appropriate tiles per zoom_level for the viewport

        BeginDrawing();
        ClearBackground(RAYWHITE);

        // Draw texture centered on screen
        DrawTexture(texture,
            GetScreenWidth() / 2 - texture.width / 2,
            GetScreenHeight() / 2 - texture.height / 2,
            WHITE);

        EndDrawing();
    }

    curl_easy_cleanup(curl_handle);
    free(chunk.memory);

    return 0;
}
