#include "cumulative_transience.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char** argv) {
    printf("Mock External: Starting analysis...\n");

    int sr = 44100;
    int seconds = 5;
    int len = sr * seconds;
    float* y = (float*)malloc(sizeof(float) * len);

    for (int i = 0; i < len; i++) {
        y[i] = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
    }

    FullAnalysisResult res;
    if (analyzer_batch_analyze(y, len, sr, &res)) {
        printf("Analysis complete. Processed %d frames.\n", res.num_frames);
        printf("Max peak value: %.4f\n", res.max_peak_value);

        for (int b = 0; b < MAX_BANDS; b++) {
            printf("Band %d: %d peaks detected.\n", b, res.bands[b].num_peaks);
        }

        int last = res.num_frames - 1;
        printf("Final Rating: %.4f\n", res.ratings[last]);
        printf("Final Std Dev: %.4f\n", res.std_devs[last]);

        analyzer_free_analysis(&res);
        printf("Cleanup complete.\n");
    } else {
        printf("Analysis failed.\n");
    }

    free(y);
    return 0;
}
