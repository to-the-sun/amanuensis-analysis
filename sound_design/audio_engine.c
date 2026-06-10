#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <json-c/json.h>
#include "sound_design.h"
#include "analysis_utils.h"

int main(int argc, char** argv) {
    double duration = 5.0;
    int sr = 44100;
    const char* output_wav = "design_output.wav";

    char sounds_dir[] = "sounds";
    char version_str[16];
    sprintf(version_str, "%d", SOUND_DESIGN_VERSION);

    char subfolder[256];
    sprintf(subfolder, "%s/%s", sounds_dir, version_str);

    mkdir(sounds_dir, 0755);
    mkdir(subfolder, 0755);

    printf("Rendering %.1f seconds of audio using sound_design sandbox...\n", duration);
    int num_samples;
    double* audio = render_midi(DEFAULT_MIDI_SEQUENCE, DEFAULT_MIDI_SEQUENCE_LEN, duration, sr, &num_samples);

    char output_path[512];
    sprintf(output_path, "%s/%s", subfolder, output_wav);
    save_wav(output_path, audio, num_samples, sr);
    printf("Audio saved to %s\n", output_path);

    char cmd[1024];
    sprintf(cmd, "cp sound_design.c sound_design.h %s/", subfolder);
    system(cmd);

    struct json_object* results = analyze_audio(audio, num_samples, sr);

    struct json_object* distances = json_object_new_object();
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(sounds_dir)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_DIR && strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0 && strcmp(ent->d_name, version_str) != 0) {
                char other_json_path[512];
                sprintf(other_json_path, "%s/%s/analysis.json", sounds_dir, ent->d_name);
                struct json_object* other_results = json_object_from_file(other_json_path);
                if (other_results) {
                    double dist = calculate_distance(results, other_results);
                    json_object_object_add(distances, ent->d_name, json_object_new_double(dist));
                    json_object_put(other_results);
                }
            }
        }
        closedir(dir);
    }

    if (json_object_object_length(distances) > 0) {
        json_object_object_add(results, "distances", distances);
    } else {
        json_object_put(distances);
    }

    char json_path[512];
    sprintf(json_path, "%s/analysis.json", subfolder);
    json_object_to_file_ext(json_path, results, JSON_C_TO_STRING_PRETTY);
    printf("Analysis saved to %s\n", json_path);

    json_object_put(results);
    free(audio);
    return 0;
}
