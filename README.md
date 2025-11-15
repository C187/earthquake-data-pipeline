# Earthquake Data Pipeline

This project fetches the most recent 24 hours of earthquake information from the [USGS GeoJSON feed](https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/all_day.geojson), normalizes the data, and stores two CSV outputs.

## Building

```bash
cmake -S . -B build
cmake --build build
```

The resulting executable is located at `build/earthquake_pipeline`.

## Running

Execute the program after building:

```bash
./build/earthquake_pipeline
```

On each run the program will:

1. Download the GeoJSON feed.
2. Parse the features to extract the timestamp, magnitude, place, longitude, latitude, and depth.
3. Append the normalized rows to `data/earthquakes.csv`, creating the directory and file when needed.
4. Produce `data/report.csv` summarizing the number of earthquakes in fixed magnitude buckets for the current run.

Both CSV files are stored in the `data/` directory, which is created automatically.

