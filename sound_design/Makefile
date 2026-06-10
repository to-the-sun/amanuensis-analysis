CC = gcc
CFLAGS = -Wall -O3 -fPIC
LIBS = -lsndfile -laubio -ljson-c -lm -ldl

all: audio_engine migrate_analysis

audio_engine: audio_engine.c sound_design.c analysis_utils.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

migrate_analysis: migrate_analysis.c analysis_utils.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f audio_engine migrate_analysis
