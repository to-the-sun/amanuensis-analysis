#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <dlfcn.h>
#include <json-c/json.h>
#include "sound_design.h"
#include "analysis_utils.h"

int main() {
    char sounds_dir[] = "sounds";
    DIR *dir;
    struct dirent *ent;
    struct json_object* all_results = json_object_new_object();

    if ((dir = opendir(sounds_dir)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_DIR && strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                printf("Processing sound %s...\n", ent->d_name);
                char folder_path[512]; sprintf(folder_path, "%s/%s", sounds_dir, ent->d_name);
                char src_path[512]; sprintf(src_path, "%s/sound_design.c", folder_path);
                if (access(src_path, F_OK) == -1) { printf("Skipping %s, sound_design.c not found.\n", ent->d_name); continue; }

                char cmd[1024];
                sprintf(cmd, "gcc -fPIC -shared -o %s/libsd.so %s -lm", folder_path, src_path);
                if (system(cmd) != 0) continue;

                char lib_path[512]; sprintf(lib_path, "./%s/libsd.so", folder_path);
                void *handle = dlopen(lib_path, RTLD_NOW);
                if (!handle) { fprintf(stderr, "%s\n", dlerror()); continue; }

                double* (*render_midi_ptr)(MidiMessage*, int, double, int, int*);
                render_midi_ptr = dlsym(handle, "render_midi");
                if (render_midi_ptr) {
                    int num_samples;
                    double* audio = render_midi_ptr(DEFAULT_MIDI_SEQUENCE, DEFAULT_MIDI_SEQUENCE_LEN, 5.0, 44100, &num_samples);
                    json_object_object_add(all_results, ent->d_name, analyze_audio(audio, num_samples, 44100));
                    free(audio);
                }
                dlclose(handle); unlink(lib_path);
            }
        }
        closedir(dir);
    }

    json_object_object_foreach(all_results, key, val) {
        struct json_object* distances = json_object_new_object();
        json_object_object_foreach(all_results, other_key, other_val) {
            if (strcmp(key, other_key) != 0) json_object_object_add(distances, other_key, json_object_new_double(calculate_distance(val, other_val)));
        }
        json_object_object_add(val, "distances", distances);
        char json_path[512]; sprintf(json_path, "%s/%s/analysis.json", sounds_dir, key);
        json_object_to_file_ext(json_path, val, JSON_C_TO_STRING_PRETTY);
        printf("Updated %s\n", json_path);
    }
    json_object_put(all_results);
    return 0;
}
