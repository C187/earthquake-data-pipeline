#include "json.hpp"

#include <curl/curl.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using simplejson::JsonArray;
using simplejson::JsonObject;
using simplejson::JsonValue;

namespace {

constexpr const char *kFeedUrl = "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/all_day.geojson";

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    auto *buffer = static_cast<std::string *>(userp);
    buffer->append(static_cast<char *>(contents), real_size);
    return real_size;
}

std::string fetch_feed(const std::string &url) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "earthquake-data-pipeline/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        std::string message = std::string("Failed to fetch feed: ") + curl_easy_strerror(result);
        curl_easy_cleanup(curl);
        throw std::runtime_error(message);
    }

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    if (response_code >= 400) {
        std::ostringstream oss;
        oss << "HTTP error " << response_code;
        throw std::runtime_error(oss.str());
    }

    return response;
}

struct Record {
    std::string time_iso;
    std::optional<double> magnitude;
    std::string place;
    std::optional<double> longitude;
    std::optional<double> latitude;
    std::optional<double> depth_km;
};

std::string iso8601_from_millis(int64_t millis_since_epoch) {
    using namespace std::chrono;
    system_clock::time_point tp = system_clock::time_point(milliseconds(millis_since_epoch));
    std::time_t tt = system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    if (!gmtime_r(&tt, &tm)) {
        throw std::runtime_error("Failed to convert timestamp to UTC");
    }
#endif
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()).count() % 1000;
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
    return oss.str();
}

std::optional<double> to_number(const JsonValue *value) {
    if (!value) {
        return std::nullopt;
    }
    if (value->is_null()) {
        return std::nullopt;
    }
    if (value->is_number()) {
        return value->as_number();
    }
    return std::nullopt;
}

std::optional<std::string> to_string(const JsonValue *value) {
    if (!value) {
        return std::nullopt;
    }
    if (value->is_null()) {
        return std::nullopt;
    }
    if (value->is_string()) {
        return value->as_string();
    }
    return std::nullopt;
}

std::vector<Record> parse_records(const std::string &payload) {
    JsonValue root = simplejson::parse(payload);
    if (!root.is_object()) {
        throw std::runtime_error("Unexpected JSON root type");
    }

    const JsonObject &root_obj = root.as_object();
    const JsonValue *features_value = simplejson::get(root_obj, "features");
    if (!features_value || !features_value->is_array()) {
        throw std::runtime_error("Missing features array");
    }

    const JsonArray &features = features_value->as_array();
    std::vector<Record> records;
    records.reserve(features.size());

    for (const JsonValue &feature_value : features) {
        if (!feature_value.is_object()) {
            continue;
        }
        const JsonObject &feature_obj = feature_value.as_object();

        const JsonValue *properties_value = simplejson::get(feature_obj, "properties");
        const JsonValue *geometry_value = simplejson::get(feature_obj, "geometry");
        if (!properties_value || !properties_value->is_object()) {
            continue;
        }
        if (!geometry_value || !geometry_value->is_object()) {
            continue;
        }

        const JsonObject &properties = properties_value->as_object();
        const JsonObject &geometry = geometry_value->as_object();

        const JsonValue *time_value = simplejson::get(properties, "time");
        if (!time_value || !time_value->is_number()) {
            continue;
        }

        Record record;
        int64_t millis = static_cast<int64_t>(time_value->as_number());
        record.time_iso = iso8601_from_millis(millis);

        record.magnitude = to_number(simplejson::get(properties, "mag"));
        record.place = to_string(simplejson::get(properties, "place")).value_or("");

        const JsonValue *coordinates_value = simplejson::get(geometry, "coordinates");
        if (coordinates_value && coordinates_value->is_array()) {
            const JsonArray &coordinates = coordinates_value->as_array();
            record.longitude = to_number(simplejson::get(coordinates, 0));
            record.latitude = to_number(simplejson::get(coordinates, 1));
            record.depth_km = to_number(simplejson::get(coordinates, 2));
        }

        records.push_back(std::move(record));
    }

    return records;
}

std::string escape_csv(const std::string &value) {
    bool needs_quotes = value.find_first_of(",\"\n") != std::string::npos;
    if (!needs_quotes) {
        return value;
    }
    std::string escaped = "\"";
    for (char ch : value) {
        if (ch == '\"') {
            escaped.push_back('\"');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('\"');
    return escaped;
}

void append_records_to_csv(const std::vector<Record> &records, const std::filesystem::path &path) {
    bool file_exists = std::filesystem::exists(path);
    std::ofstream out(path, std::ios::app);
    if (!out) {
        throw std::runtime_error("Failed to open earthquakes.csv for writing");
    }
    if (!file_exists) {
        out << "time_iso,magnitude,place,longitude,latitude,depth_km\n";
    }
    for (const auto &record : records) {
        out << record.time_iso << ',';
        if (record.magnitude) {
            out << *record.magnitude;
        }
        out << ',' << escape_csv(record.place) << ',';
        if (record.longitude) {
            out << *record.longitude;
        }
        out << ',';
        if (record.latitude) {
            out << *record.latitude;
        }
        out << ',';
        if (record.depth_km) {
            out << *record.depth_km;
        }
        out << "\n";
    }
}

struct Bucket {
    double min_inclusive;
    double max_exclusive;
    std::string label;
};

std::vector<Bucket> make_buckets() {
    return {
        {std::numeric_limits<double>::lowest(), 1.0, "<1.0"},
        {1.0, 2.0, "1.0-1.9"},
        {2.0, 3.0, "2.0-2.9"},
        {3.0, 4.0, "3.0-3.9"},
        {4.0, 5.0, "4.0-4.9"},
        {5.0, 6.0, "5.0-5.9"},
        {6.0, 7.0, "6.0-6.9"},
        {7.0, 8.0, "7.0-7.9"},
        {8.0, std::numeric_limits<double>::infinity(), ">=8.0"}
    };
}

void write_report(const std::vector<Record> &records, const std::filesystem::path &path) {
    std::vector<Bucket> buckets = make_buckets();
    std::vector<std::size_t> counts(buckets.size(), 0);

    for (const auto &record : records) {
        if (!record.magnitude) {
            continue;
        }
        double mag = *record.magnitude;
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            const Bucket &bucket = buckets[i];
            if (mag >= bucket.min_inclusive && mag < bucket.max_exclusive) {
                counts[i] += 1;
                break;
            }
        }
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open report.csv for writing");
    }
    out << "range,count\n";
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        out << buckets[i].label << ',' << counts[i] << "\n";
    }
}

class CurlGlobal {
public:
    CurlGlobal() {
        CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) {
            throw std::runtime_error("Failed to initialize CURL globals");
        }
    }

    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

} // namespace

int main() {
    try {
        CurlGlobal curl_initializer;

        std::string payload = fetch_feed(kFeedUrl);
        std::vector<Record> records = parse_records(payload);

        if (records.empty()) {
            std::cerr << "No earthquake records found.\n";
        }

        std::filesystem::create_directories("data");
        append_records_to_csv(records, "data/earthquakes.csv");
        write_report(records, "data/report.csv");

        std::cout << "Processed " << records.size() << " earthquake events." << std::endl;
        return 0;
    } catch (const simplejson::ParseError &ex) {
        std::cerr << "JSON parse error: " << ex.what() << std::endl;
        return 1;
    } catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
        return 1;
    }
}

