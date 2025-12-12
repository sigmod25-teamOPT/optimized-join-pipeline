#pragma once

#include <filesystem>
#include <fmt/core.h>
#include <range/v3/all.hpp>

#include <attribute.h>
#include <plan.h>
#include <statement.h>

#ifdef TEAMOPT_USE_DUCKDB
#include <duckdb.hpp>
#endif

namespace FNVHash {
    constexpr uint64_t FNV_prime = 1099511628211u;
    constexpr uint64_t offset_basis = 14695981039346656037u;

    inline uint64_t hash(const void* key, size_t len) {
        uint64_t h = offset_basis;
        const unsigned char* p = static_cast<const unsigned char*>(key);
        for (size_t i = 0; i < len; ++i) {
            h ^= static_cast<uint64_t>(p[i]);
            h *= FNV_prime;
        }
        return h;
    }
};

struct TableMeta {
    uint64_t num_rows;
    uint64_t num_cols;
    DataType types[16];
    uint64_t num_pages[16];
};

#define FILLER_SIZE (PAGE_SIZE - sizeof(struct TableMeta))

struct Table {
public:
    Table() = default;

    Table(std::vector<std::vector<Data>> data, std::vector<DataType> types)
    : types_(types)
    , data_(data) {}


    static ColumnarTable from_cache(const std::filesystem::path& path);

    static ColumnarTable from_csv(const std::vector<Attribute>& attributes,
        const std::filesystem::path&                            path,
        Statement*                                              filter,
        bool                                                    header = false);

    static Table from_columnar(const ColumnarTable& input);

    static std::vector<std::vector<Data>> copy_scan(const ColumnarTable& table,
                const std::vector<std::tuple<size_t, DataType>>& output_attrs);

    ColumnarTable to_columnar() const;

    const std::vector<std::vector<Data>>& table() const { return data_; }

    std::vector<std::vector<Data>>& table() { return data_; }

    const std::vector<DataType>& types() const { return types_; }

    size_t number_rows() const { return this->data_.size(); }

    size_t number_cols() const { return this->types_.size(); }

    static void print(const std::vector<std::vector<Data>>& data) {
        namespace views = ranges::views;

        auto escape_string = [](const std::string& s) {
            std::string escaped;
            for (char c: s) {
                switch (c) {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:   escaped += c; break;
                }
            }
            return escaped;
        };

        for (auto& record: data) {
            auto line = record
                      | views::transform([&escape_string](const Data& field) -> std::string {
                            return std::visit(
                                [&escape_string](const auto& arg) {
                                    using T = std::decay_t<decltype(arg)>;
                                    using namespace std::string_literals;
                                    if constexpr (std::is_same_v<T, std::monostate>) {
                                        return "NULL"s;
                                    } else if constexpr (std::is_same_v<T, int32_t>
                                                         || std::is_same_v<T, int64_t>
                                                         || std::is_same_v<T, double>) {
                                        return fmt::format("{}", arg);
                                    } else if constexpr (std::is_same_v<T, std::string>) {
                                        return fmt::format("\"{}\"", escape_string(arg));
                                        // return fmt::format("{}", arg);
                                    }
                                },
                                field);
                        })
                      | views::join('|') | ranges::to<std::string>();
            fmt::println("{}", line);
        }
    }

private:
    std::vector<DataType>          types_;
    std::vector<std::vector<Data>> data_;

    void set_attributes(const std::vector<Attribute>& attributes) {
        this->types_.clear();
        for (auto& attr: attributes) {
            this->types_.push_back(attr.type);
        }
    }
};


class DumpTable {
    private:
    TableMeta tablemeta = {0};
    ColumnarTable *table;

    public:
    DumpTable(ColumnarTable *table): table(table) {
        tablemeta.num_rows = table->num_rows;
        tablemeta.num_cols = table->columns.size();
        for (size_t i = 0; i < tablemeta.num_cols; ++i) {
            tablemeta.types[i] = table->columns[i].type;
            tablemeta.num_pages[i] = table->columns[i].pages.size();
        }
    }
#ifdef TEAMOPT_USE_DUCKDB
    DumpTable(duckdb::MaterializedQueryResult &duckdb_results) {
        std::vector<std::vector<Data>> data;
        std::vector<DataType>          types;

        /* Setup metadata */
        tablemeta.num_rows = duckdb_results.RowCount();
        tablemeta.num_cols = duckdb_results.ColumnCount();
        for (size_t i = 0; i < tablemeta.num_cols; ++i) {
            switch (duckdb_results.types[i].id()) {
                case duckdb::LogicalTypeId::INTEGER:
                    tablemeta.types[i] = DataType::INT32;
                    types.push_back(DataType::INT32);
                    break;
                case duckdb::LogicalTypeId::VARCHAR:
                    tablemeta.types[i] = DataType::VARCHAR;
                    types.push_back(DataType::VARCHAR);
                    break;
                default:
                    throw std::runtime_error(fmt::format("{} in DuckDB is not supported",
                                            duckdb_results.types[i].ToString()));
            }
        }

        /* Convert duckdb to ColumnarTable */
        auto& coll = duckdb_results.Collection();
        for (auto& row: coll.Rows()) {
            std::vector<Data> record;
            for (size_t col_idx = 0; col_idx < tablemeta.num_cols; col_idx++) {
                auto val = row.GetValue(col_idx);
                if (val.IsNull()) {
                    record.emplace_back(std::monostate{});
                } else {
                    switch (types[col_idx]) {
                    case DataType::INT32: {
                        record.emplace_back(duckdb::IntegerValue::Get(val));
                        break;
                    }
                    case DataType::VARCHAR: {
                        record.emplace_back(duckdb::StringValue::Get(val));
                        break;
                    }
                    default:
                        throw std::runtime_error("DataType not supported");
                    }
                }
            }
            data.emplace_back(std::move(record));
        }
        std::sort(data.begin(), data.end());
        table = new ColumnarTable();
        *table = Table(data, types).to_columnar();

        for (size_t i = 0 ; i < tablemeta.num_cols ; ++i) {
            tablemeta.num_pages[i] = table->columns[i].pages.size();
        }
    }
#endif

    void dump(std::ostream& out) {
        out.write(reinterpret_cast<const char*>(&tablemeta), sizeof(struct TableMeta));
        char filler[FILLER_SIZE] = {0};
        out.write(filler, FILLER_SIZE);

        for (size_t i = 0; i < tablemeta.num_cols; ++i) {
            for (auto* page: table->columns[i].pages) {
                out.write(reinterpret_cast<const char*>(page->data), PAGE_SIZE);
            }
        }
    }
};