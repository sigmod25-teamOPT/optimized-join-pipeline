#include <atomic>
#include <charconv>

#include <common.h>
#include <csv_parser.h>
#include <inner_column.h>
#include <plan.h>
#include <table.h>

#if !defined(TEAMOPT_USE_DUCKDB) || defined(TEAMOPT_BUILD_CACHE)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

template <class Functor>
class TableParser: public CSVParser {
public:
    size_t            row_off_;
    std::vector<Data> last_record_;
    const Attribute*  attributes_data_;
    size_t            attributes_size_;
    Functor           add_record_fn_;

    template <class F>
    TableParser(const std::vector<Attribute>& attributes,
        F&&                                   functor_,
        char                                  escape = '"',
        char sep                                     = ',',
        bool has_trailing_comma                      = false,
        bool has_header                              = false)
    : CSVParser(escape, sep, has_trailing_comma)
    , attributes_data_(attributes.data())
    , attributes_size_(attributes.size())
    , row_off_(has_header ? static_cast<size_t>(-1) : static_cast<size_t>(0))
    , add_record_fn_(static_cast<F&&>(functor_)) {}

    void on_field(size_t col_idx, size_t row_idx, const char* begin, size_t len) override {
        if (row_idx + this->row_off_ == static_cast<size_t>(-1)) {
            return;
        }
        if (len == 0) {
            this->last_record_.emplace_back(std::monostate{});
        } else {
            switch (this->attributes_data_[col_idx].type) {
            case DataType::INT32: {
            int32_t value;
            try {
                value = std::stoi(std::string(begin, len));
            } catch (const std::exception&) {
                throw std::runtime_error("parse integer error");
            }
            this->last_record_.emplace_back(value);
            break;
            }
            case DataType::INT64: {
            int64_t value;
            try {
                value = std::stoll(std::string(begin, len));
            } catch (const std::exception&) {
                throw std::runtime_error("parse integer error");
            }
            this->last_record_.emplace_back(value);
            break;
            }
            case DataType::FP64: {
            double value;
            try {
                value = std::stod(std::string(begin, len));
            } catch (const std::exception&) {
                throw std::runtime_error("parse float error");
            }
                this->last_record_.emplace_back(value);
                break;
            }
            case DataType::VARCHAR: {
                this->last_record_.emplace_back(std::string{begin, len});
                break;
            }
            }
        }
        if (col_idx + 1 == this->attributes_size_) {
            this->add_record_fn_(std::move(this->last_record_));
            this->last_record_.clear();
            this->last_record_.reserve(attributes_size_);
        }
    }
};

template <class F>
TableParser(const std::vector<Attribute>& attributes,
    F&&                                   functor_,
    char                                  escape = '"',
    char sep                                     = ',',
    bool has_trailing_comma                      = false,
    bool has_header                              = false) -> TableParser<std::decay_t<F>>;

char buffer[1024 * 1024];

std::unordered_map<std::string, InnerTable>    table_cache;
std::unordered_map<std::string, ColumnarTable> result_cache;

template <class T>
size_t from_inner_to_column(const InnerColumnBase* inner,
    Column&                                        column,
    const std::vector<uint8_t>&                    results,
    size_t                                         rows) {
    auto*  c        = reinterpret_cast<const InnerColumn<T>*>(inner);
    auto   inserter = ColumnInserter<T>(column);
    size_t ret      = 0;
    bool   first    = true;
    for (size_t i = 0; i < rows; ++i) {
        size_t  byte_idx    = i / 8;
        size_t  bit_idx     = i % 8;
        uint8_t eval_result = results[byte_idx] & (0x1 << bit_idx);
        if (eval_result) {
            if (c->is_not_null(i)) {
                auto value = c->get(i);
                inserter.insert(value);
            } else {
                inserter.insert_null();
            }
            ++ret;
        }
    }
    inserter.finalize();
    return ret;
}

ColumnarTable copy(const ColumnarTable& value) {
    ColumnarTable ret;
    ret.num_rows = value.num_rows;
    for (auto& column: value.columns) {
        ret.columns.emplace_back(column.type);
        auto& last_column = ret.columns.back();
        for (auto* page: column.pages) {
            auto* last_page = last_column.new_page();
            memcpy(last_page->data, page->data, PAGE_SIZE);
        }
    }
    return ret;
}

#if !defined(TEAMOPT_USE_DUCKDB) || defined(TEAMOPT_BUILD_CACHE)

ColumnarTable Table::from_cache(const std::filesystem::path& path) {
    // mmap file
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }
    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        close(fd);
        throw std::runtime_error("Failed to stat file: " + path.string());
    }
    if (sb.st_size == 0) {
        close(fd);
        throw std::runtime_error("File is empty: " + path.string());
    }
#ifdef MAP_POPULATE
    void* file_in_memory = mmap(nullptr, sb.st_size, PROT_READ,
                                MAP_PRIVATE | MAP_POPULATE, fd, 0);
    if (file_in_memory == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("Failed to mmap file: " + path.string());
    }
#else
    void* file_in_memory = mmap(nullptr, sb.st_size, PROT_READ,
                                MAP_PRIVATE, fd, 0);
    if (file_in_memory == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("Failed to mmap file: " + path.string());
    }
    // Touch each page to force it into memory
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096; // fallback to 4096 if sysconf fails
    }
    for (size_t offset = 0; offset < static_cast<size_t>(sb.st_size); offset += page_size) {
        volatile char c = *(reinterpret_cast<char*>(file_in_memory) + offset);
        (void)c;
    }
#endif
    close(fd);
    // Now create a columnar table from file
    MappedMemory* mapped_memory = new MappedMemory(file_in_memory, sb.st_size);
    std::vector<Column> columns;
    TableMeta *meta = reinterpret_cast<TableMeta*>(file_in_memory);
    std::byte* data = reinterpret_cast<std::byte*>(file_in_memory) + PAGE_SIZE;
    for (size_t i = 0; i < meta->num_cols; ++i) {
        columns.emplace_back(meta->types[i]);
        auto& last_column = columns.back();
        last_column.assign_mapped_memory(mapped_memory);
        for (size_t j = 0; j < meta->num_pages[i]; ++j) {
            Page *page = reinterpret_cast<Page*>(data);
            last_column.pages.push_back(page);
            data += PAGE_SIZE;
        }
    }
    return ColumnarTable{meta->num_rows, std::move(columns)};
}

#endif

ColumnarTable Table::from_csv(const std::vector<Attribute>& attributes,
    const std::filesystem::path&                            path,
    Statement*                                              filter,
    bool                                                    header) {
    namespace views = ranges::views;
    InnerTableView                    table;
    std::vector<std::vector<Data>>    ground_truth;
    decltype(result_cache.find(path.c_str())) result_itr;
    if (not filter
        and (result_itr = result_cache.find(path.c_str()), result_itr != result_cache.end())) {
        // fmt::println("    result cache hit");
        return copy(result_itr->second);
    }
    if (auto itr = table_cache.find(path.c_str()); itr != table_cache.end()) {
        // fmt::println("    cache hit");
        table = itr->second;
    } else {
        // fmt::println("    cache miss");
        InnerTable full_table;
        full_table.rows = 0;
        for (const auto& attr: attributes) {
            switch (attr.type) {
            case DataType::INT32: {
                full_table.columns.emplace_back(std::make_unique<InnerColumn<int32_t>>());
                break;
            }
            case DataType::INT64: {
                full_table.columns.emplace_back(std::make_unique<InnerColumn<int64_t>>());
                break;
            }
            case DataType::FP64: {
                full_table.columns.emplace_back(std::make_unique<InnerColumn<double>>());
                break;
            }
            case DataType::VARCHAR: {
                full_table.columns.emplace_back(std::make_unique<InnerColumn<std::string>>());
                break;
            }
            }
        }
        File fp(path, "rb");
        auto add_record = [&ground_truth, &full_table, &attributes, filter](
                              std::vector<Data>&& record) {
            full_table.rows += 1;
            for (const auto& [idx, field]: record | views::enumerate) {
                switch (attributes[idx].type) {
                case DataType::INT32: {
                    auto* column =
                        reinterpret_cast<InnerColumn<int32_t>*>(full_table.columns[idx].get());
                    auto* data = std::get_if<int32_t>(&field);
                    if (data) {
                        column->push_back(*data);
                    } else {
                        column->push_back_null();
                    }
                    break;
                }
                case DataType::INT64: {
                    auto* column =
                        reinterpret_cast<InnerColumn<int64_t>*>(full_table.columns[idx].get());
                    auto* data = std::get_if<int64_t>(&field);
                    if (data) {
                        column->push_back(*data);
                    } else {
                        column->push_back_null();
                    }
                    break;
                }
                case DataType::FP64: {
                    auto* column =
                        reinterpret_cast<InnerColumn<double>*>(full_table.columns[idx].get());
                    auto* data = std::get_if<double>(&field);
                    if (data) {
                        column->push_back(*data);
                    } else {
                        column->push_back_null();
                    }
                    break;
                }
                case DataType::VARCHAR: {
                    auto* column = reinterpret_cast<InnerColumn<std::string>*>(
                        full_table.columns[idx].get());
                    auto* data = std::get_if<std::string>(&field);
                    if (data) {
                        column->push_back(*data);
                    } else {
                        column->push_back_null();
                    }
                    break;
                }
                }
            }
        };
        TableParser parser(attributes, std::move(add_record), '\\', ',', false, header);
        while (true) {
            auto bytes_read = fread(buffer, 1, sizeof(buffer), fp);
            if (bytes_read != 0) {
                auto err = parser.execute(buffer, bytes_read);
                if (err != CSVParser::Ok) {
                    throw std::runtime_error("CSV parse error");
                }
            } else {
                break;
            }
        }
        auto err = parser.finish();
        if (err != CSVParser::Ok) {
            throw std::runtime_error("CSV parse error");
        }
        auto [iter, _] = table_cache.emplace(path.c_str(), std::move(full_table));
        table          = iter->second;
    }
    ColumnarTable        ret;
    std::vector<uint8_t> results;
    if (filter) {
        results = filter->eval(table.columns);
    } else {
        results.resize((table.rows + 7) / 8, 0xff);
    }
    ret.num_rows = 0;
    for (auto* column: table.columns) {
        ret.columns.emplace_back(column->type);
    }
    std::atomic_size_t ret_rows;
    auto               task = [&table, &ret, &results, &ret_rows](size_t begin, size_t end) {
        for (size_t column_idx = begin; column_idx < end; ++column_idx) {
            auto* column = table.columns[column_idx];
            switch (column->type) {
            case DataType::INT32: {
                auto filtered_rows = from_inner_to_column<int32_t>(column,
                    ret.columns[column_idx],
                    results,
                    table.rows);
                ret_rows.store(filtered_rows, std::memory_order_relaxed);
                break;
            }
            case DataType::INT64: {
                auto filtered_rows = from_inner_to_column<int64_t>(column,
                    ret.columns[column_idx],
                    results,
                    table.rows);
                ret_rows.store(filtered_rows, std::memory_order_relaxed);
                break;
            }
            case DataType::FP64: {
                auto filtered_rows = from_inner_to_column<double>(column,
                    ret.columns[column_idx],
                    results,
                    table.rows);
                ret_rows.store(filtered_rows, std::memory_order_relaxed);
                break;
            }
            case DataType::VARCHAR: {
                auto filtered_rows = from_inner_to_column<std::string>(column,
                    ret.columns[column_idx],
                    results,
                    table.rows);
                ret_rows.store(filtered_rows, std::memory_order_relaxed);
                break;
            }
            }
        }
    };
    filter_tp.run(task, table.columns.size());
    ret.num_rows = ret_rows.load(std::memory_order_relaxed);
    if (not filter) {
        result_cache.emplace(path.c_str(), copy(ret));
    }
    return ret;
}

bool get_bitmap(const uint8_t* bitmap, uint16_t idx) {
    auto byte_idx = idx / 8;
    auto bit      = idx % 8;
    return bitmap[byte_idx] & (1u << bit);
}

std::vector<std::vector<Data>> Table::copy_scan(const ColumnarTable& table,
     const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    namespace views = ranges::views;
    std::vector<std::vector<Data>> results(table.num_rows,
        std::vector<Data>(output_attrs.size(), std::monostate{}));
    std::vector<DataType>          types(table.columns.size());
    auto task = [&](size_t begin, size_t end) {
        size_t col_pap = 0;
        for (size_t column_idx = begin; column_idx < end; ++column_idx) {
            size_t in_col_idx = std::get<0>(output_attrs[column_idx]);
            auto& column = table.columns[in_col_idx];
            types[in_col_idx] = column.type;
            size_t row_idx = 0;
            for (auto* page:
                column.pages | views::transform([](auto* page) { return page->data; })) {
                switch (column.type) {
                case DataType::INT32: {
                    auto  num_rows   = *reinterpret_cast<uint16_t*>(page);
                    auto* data_begin = reinterpret_cast<int32_t*>(page + 4);
                    auto* bitmap =
                        reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows; ++i) {
                        if (get_bitmap(bitmap, i)) {
                            auto value = data_begin[data_idx++];
                            if (row_idx >= table.num_rows) {
                                throw std::runtime_error("row_idx");
                            }
                            results[row_idx++][column_idx].emplace<int32_t>(value);
                        } else {
                            ++row_idx;
                        }
                    }
                    break;
                }
                case DataType::INT64: {
                    auto  num_rows   = *reinterpret_cast<uint16_t*>(page);
                    auto* data_begin = reinterpret_cast<int64_t*>(page + 8);
                    auto* bitmap =
                        reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows; ++i) {
                        if (get_bitmap(bitmap, i)) {
                            auto value = data_begin[data_idx++];
                            if (row_idx >= table.num_rows) {
                                throw std::runtime_error("row_idx");
                            }
                            results[row_idx++][column_idx].emplace<int64_t>(value);
                        } else {
                            ++row_idx;
                        }
                    }
                    break;
                }
                case DataType::FP64: {
                    auto  num_rows   = *reinterpret_cast<uint16_t*>(page);
                    auto* data_begin = reinterpret_cast<double*>(page + 8);
                    auto* bitmap =
                        reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows; ++i) {
                        if (get_bitmap(bitmap, i)) {
                            auto value = data_begin[data_idx++];
                            if (row_idx >= table.num_rows) {
                                throw std::runtime_error("row_idx");
                            }
                            results[row_idx++][column_idx].emplace<double>(value);
                        } else {
                            ++row_idx;
                        }
                    }
                    break;
                }
                case DataType::VARCHAR: {
                    auto num_rows = *reinterpret_cast<uint16_t*>(page);
                    if (num_rows == 0xffff) {
                        auto        num_chars  = *reinterpret_cast<uint16_t*>(page + 2);
                        auto*       data_begin = reinterpret_cast<char*>(page + 4);
                        std::string value{data_begin, data_begin + num_chars};
                        if (row_idx >= table.num_rows) {
                            throw std::runtime_error("row_idx");
                        }
                        results[row_idx++][column_idx].emplace<std::string>(std::move(value));
                    } else if (num_rows == 0xfffe) {
                        auto  num_chars  = *reinterpret_cast<uint16_t*>(page + 2);
                        auto* data_begin = reinterpret_cast<char*>(page + 4);
                        std::visit(
                            [data_begin, num_chars](auto& value) {
                                using T = std::decay_t<decltype(value)>;
                                if constexpr (std::is_same_v<T, std::string>) {
                                    value.insert(value.end(), data_begin, data_begin + num_chars);
                                } else {
                                    throw std::runtime_error(
                                        "long string page 0xfffe must follows a string");
                                }
                            },
                            results[row_idx - 1][column_idx]);
                    } else {
                        auto  num_non_null = *reinterpret_cast<uint16_t*>(page + 2);
                        auto* offset_begin = reinterpret_cast<uint16_t*>(page + 4);
                        auto* data_begin   = reinterpret_cast<char*>(page + 4 + num_non_null * 2);
                        auto* string_begin = data_begin;
                        auto* bitmap =
                            reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                        uint16_t data_idx = 0;
                        for (uint16_t i = 0; i < num_rows; ++i) {
                            if (get_bitmap(bitmap, i)) {
                                auto        offset = offset_begin[data_idx++];
                                std::string value{string_begin, data_begin + offset};
                                string_begin = data_begin + offset;
                                if (row_idx >= table.num_rows) {
                                    throw std::runtime_error("row_idx");
                                }
                                results[row_idx++][column_idx].emplace<std::string>(std::move(value));
                            } else {
                                ++row_idx;
                            }
                        }
                    }
                    break;
                }
                }
            }
        }
    };
    filter_tp.run(task, output_attrs.size());
    return results;
}

Table Table::from_columnar(const ColumnarTable& table) {
    namespace views = ranges::views;
    std::vector<std::vector<Data>> results(table.num_rows,
        std::vector<Data>(table.columns.size(), std::monostate{}));
    std::vector<DataType>          types(table.columns.size());
    auto task = [&](size_t begin, size_t end) {
        for (size_t column_idx = begin; column_idx < end; ++column_idx) {
            auto& column = table.columns[column_idx];
            types[column_idx] = column.type;
            size_t row_idx = 0;
            for (auto* page:
                column.pages | views::transform([](auto* page) { return page->data; })) {
                switch (column.type) {
                case DataType::INT32: {
                    auto  num_rows   = *reinterpret_cast<uint16_t*>(page);
                    auto* data_begin = reinterpret_cast<int32_t*>(page + 4);
                    auto* bitmap =
                        reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows; ++i) {
                        if (get_bitmap(bitmap, i)) {
                            auto value = data_begin[data_idx++];
                            if (row_idx >= table.num_rows) {
                                throw std::runtime_error("row_idx");
                            }
                            results[row_idx++][column_idx].emplace<int32_t>(value);
                        } else {
                            ++row_idx;
                        }
                    }
                    break;
                }
                case DataType::INT64: {
                    auto  num_rows   = *reinterpret_cast<uint16_t*>(page);
                    auto* data_begin = reinterpret_cast<int64_t*>(page + 8);
                    auto* bitmap =
                        reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows; ++i) {
                        if (get_bitmap(bitmap, i)) {
                            auto value = data_begin[data_idx++];
                            if (row_idx >= table.num_rows) {
                                throw std::runtime_error("row_idx");
                            }
                            results[row_idx++][column_idx].emplace<int64_t>(value);
                        } else {
                            ++row_idx;
                        }
                    }
                    break;
                }
                case DataType::FP64: {
                    auto  num_rows   = *reinterpret_cast<uint16_t*>(page);
                    auto* data_begin = reinterpret_cast<double*>(page + 8);
                    auto* bitmap =
                        reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                    uint16_t data_idx = 0;
                    for (uint16_t i = 0; i < num_rows; ++i) {
                        if (get_bitmap(bitmap, i)) {
                            auto value = data_begin[data_idx++];
                            if (row_idx >= table.num_rows) {
                                throw std::runtime_error("row_idx");
                            }
                            results[row_idx++][column_idx].emplace<double>(value);
                        } else {
                            ++row_idx;
                        }
                    }
                    break;
                }
                case DataType::VARCHAR: {
                    auto num_rows = *reinterpret_cast<uint16_t*>(page);
                    if (num_rows == 0xffff) {
                        auto        num_chars  = *reinterpret_cast<uint16_t*>(page + 2);
                        auto*       data_begin = reinterpret_cast<char*>(page + 4);
                        std::string value{data_begin, data_begin + num_chars};
                        if (row_idx >= table.num_rows) {
                            throw std::runtime_error("row_idx");
                        }
                        results[row_idx++][column_idx].emplace<std::string>(std::move(value));
                    } else if (num_rows == 0xfffe) {
                        auto  num_chars  = *reinterpret_cast<uint16_t*>(page + 2);
                        auto* data_begin = reinterpret_cast<char*>(page + 4);
                        std::visit(
                            [data_begin, num_chars](auto& value) {
                                using T = std::decay_t<decltype(value)>;
                                if constexpr (std::is_same_v<T, std::string>) {
                                    value.insert(value.end(), data_begin, data_begin + num_chars);
                                } else {
                                    throw std::runtime_error(
                                        "long string page 0xfffe must follows a string");
                                }
                            },
                            results[row_idx - 1][column_idx]);
                    } else {
                        auto  num_non_null = *reinterpret_cast<uint16_t*>(page + 2);
                        auto* offset_begin = reinterpret_cast<uint16_t*>(page + 4);
                        auto* data_begin   = reinterpret_cast<char*>(page + 4 + num_non_null * 2);
                        auto* string_begin = data_begin;
                        auto* bitmap =
                            reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                        uint16_t data_idx = 0;
                        for (uint16_t i = 0; i < num_rows; ++i) {
                            if (get_bitmap(bitmap, i)) {
                                auto        offset = offset_begin[data_idx++];
                                std::string value{string_begin, data_begin + offset};
                                string_begin = data_begin + offset;
                                if (row_idx >= table.num_rows) {
                                    throw std::runtime_error("row_idx");
                                }
                                results[row_idx++][column_idx].emplace<std::string>(std::move(value));
                            } else {
                                ++row_idx;
                            }
                        }
                    }
                    break;
                }
                }
            }
        }
    };
    filter_tp.run(task, table.columns.size());
    return {results, types};
}

void set_bitmap(std::vector<uint8_t>& bitmap, uint16_t idx) {
    while (bitmap.size() < idx / 8 + 1) {
        bitmap.emplace_back(0);
    }
    auto byte_idx     = idx / 8;
    auto bit          = idx % 8;
    bitmap[byte_idx] |= (1u << bit);
}

void unset_bitmap(std::vector<uint8_t>& bitmap, uint16_t idx) {
    while (bitmap.size() < idx / 8 + 1) {
        bitmap.emplace_back(0);
    }
    auto byte_idx     = idx / 8;
    auto bit          = idx % 8;
    bitmap[byte_idx] &= ~(1u << bit);
}

ColumnarTable Table::to_columnar() const {
    auto& table      = this->data_;
    auto& data_types = this->types_;
    namespace views  = ranges::views;
    ColumnarTable ret;
    ret.num_rows = table.size();
    for (auto [col_idx, data_type]: data_types | views::enumerate) {
        ret.columns.emplace_back(data_type);
        auto& column = ret.columns.back();
        switch (data_type) {
        case DataType::INT32: {
            uint16_t             num_rows = 0;
            std::vector<int32_t> data;
            std::vector<uint8_t> bitmap;
            data.reserve(2048);
            bitmap.reserve(256);
            auto save_page = [&column, &num_rows, &data, &bitmap]() {
                auto* page                             = column.new_page()->data;
                *reinterpret_cast<uint16_t*>(page)     = num_rows;
                *reinterpret_cast<uint16_t*>(page + 2) = static_cast<uint16_t>(data.size());
                memcpy(page + 4, data.data(), data.size() * 4);
                memcpy(page + PAGE_SIZE - bitmap.size(), bitmap.data(), bitmap.size());
                num_rows = 0;
                data.clear();
                bitmap.clear();
            };
            for (auto& record: table) {
                auto& value = record[col_idx];
                std::visit(
                    [&save_page, &column, &num_rows, &data, &bitmap](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, int32_t>) {
                            if (4 + (data.size() + 1) * 4 + (num_rows / 8 + 1) > PAGE_SIZE) {
                                save_page();
                            }
                            set_bitmap(bitmap, num_rows);
                            data.emplace_back(value);
                            ++num_rows;
                        } else if constexpr (std::is_same_v<T, std::monostate>) {
                            if (4 + (data.size()) * 4 + (num_rows / 8 + 1) > PAGE_SIZE) {
                                save_page();
                            }
                            unset_bitmap(bitmap, num_rows);
                            ++num_rows;
                        }
                    },
                    value);
            }
            if (num_rows != 0) {
                save_page();
            }
            break;
        }
        case DataType::INT64: {
            uint16_t             num_rows = 0;
            std::vector<int64_t> data;
            std::vector<uint8_t> bitmap;
            data.reserve(1024);
            bitmap.reserve(128);
            auto save_page = [&column, &num_rows, &data, &bitmap]() {
                auto* page                             = column.new_page()->data;
                *reinterpret_cast<uint16_t*>(page)     = num_rows;
                *reinterpret_cast<uint16_t*>(page + 2) = static_cast<uint16_t>(data.size());
                memcpy(page + 8, data.data(), data.size() * 8);
                memcpy(page + PAGE_SIZE - bitmap.size(), bitmap.data(), bitmap.size());
                num_rows = 0;
                data.clear();
                bitmap.clear();
            };
            for (auto& record: table) {
                auto& value = record[col_idx];
                std::visit(
                    [&save_page, &column, &num_rows, &data, &bitmap](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, int64_t>) {
                            if (8 + (data.size() + 1) * 8 + (num_rows / 8 + 1) > PAGE_SIZE) {
                                save_page();
                            }
                            set_bitmap(bitmap, num_rows);
                            data.emplace_back(value);
                            ++num_rows;
                        } else if constexpr (std::is_same_v<T, std::monostate>) {
                            if (8 + (data.size()) * 8 + (num_rows / 8 + 1) > PAGE_SIZE) {
                                save_page();
                            }
                            unset_bitmap(bitmap, num_rows);
                            ++num_rows;
                        }
                    },
                    value);
            }
            if (num_rows != 0) {
                save_page();
            }
            break;
        }
        case DataType::FP64: {
            uint16_t             num_rows = 0;
            std::vector<double>  data;
            std::vector<uint8_t> bitmap;
            data.reserve(1024);
            bitmap.reserve(128);
            auto save_page = [&column, &num_rows, &data, &bitmap]() {
                auto* page                             = column.new_page()->data;
                *reinterpret_cast<uint16_t*>(page)     = num_rows;
                *reinterpret_cast<uint16_t*>(page + 2) = static_cast<uint16_t>(data.size());
                memcpy(page + 8, data.data(), data.size() * 8);
                memcpy(page + PAGE_SIZE - bitmap.size(), bitmap.data(), bitmap.size());
                num_rows = 0;
                data.clear();
                bitmap.clear();
            };
            for (auto& record: table) {
                auto& value = record[col_idx];
                std::visit(
                    [&save_page, &column, &num_rows, &data, &bitmap](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, double>) {
                            if (8 + (data.size() + 1) * 8 + (num_rows / 8 + 1) > PAGE_SIZE) {
                                save_page();
                            }
                            set_bitmap(bitmap, num_rows);
                            data.emplace_back(value);
                            ++num_rows;
                        } else if constexpr (std::is_same_v<T, std::monostate>) {
                            if (8 + (data.size()) * 8 + (num_rows / 8 + 1) > PAGE_SIZE) {
                                save_page();
                            }
                            unset_bitmap(bitmap, num_rows);
                            ++num_rows;
                        }
                    },
                    value);
            }
            if (num_rows != 0) {
                save_page();
            }
            break;
        }
        case DataType::VARCHAR: {
            uint16_t              num_rows = 0;
            std::vector<char>     data;
            std::vector<uint16_t> offsets;
            std::vector<uint8_t>  bitmap;
            data.reserve(8192);
            offsets.reserve(4096);
            bitmap.reserve(512);
            auto save_long_string = [&column](std::string_view data) {
                size_t offset     = 0;
                auto   first_page = true;
                while (offset < data.size()) {
                    auto* page = column.new_page()->data;
                    if (first_page) {
                        *reinterpret_cast<uint16_t*>(page) = 0xffff;
                        first_page                         = false;
                    } else {
                        *reinterpret_cast<uint16_t*>(page) = 0xfffe;
                    }
                    auto page_data_len = std::min(data.size() - offset, PAGE_SIZE - 4);
                    *reinterpret_cast<uint16_t*>(page + 2) = page_data_len;
                    memcpy(page + 4, data.data() + offset, page_data_len);
                    offset += page_data_len;
                }
            };
            auto save_page = [&column, &num_rows, &data, &offsets, &bitmap]() {
                auto* page                             = column.new_page()->data;
                *reinterpret_cast<uint16_t*>(page)     = num_rows;
                *reinterpret_cast<uint16_t*>(page + 2) = static_cast<uint16_t>(offsets.size());
                memcpy(page + 4, offsets.data(), offsets.size() * 2);
                memcpy(page + 4 + offsets.size() * 2, data.data(), data.size());
                memcpy(page + PAGE_SIZE - bitmap.size(), bitmap.data(), bitmap.size());
                num_rows = 0;
                data.clear();
                offsets.clear();
                bitmap.clear();
            };
            for (auto& record: table) {
                auto& value = record[col_idx];
                std::visit(
                    [&save_long_string,
                        &save_page,
                        &column,
                        &num_rows,
                        &data,
                        &offsets,
                        &bitmap](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, std::string>) {
                            if (value.size() > PAGE_SIZE - 7) {
                                if (num_rows > 0) {
                                    save_page();
                                }
                                save_long_string(value);
                            } else {
                                if (4 + (offsets.size() + 1) * 2 + (data.size() + value.size())
                                        + (num_rows / 8 + 1)
                                    > PAGE_SIZE) {
                                    save_page();
                                }
                                set_bitmap(bitmap, num_rows);
                                data.insert(data.end(), value.begin(), value.end());
                                offsets.emplace_back(data.size());
                                ++num_rows;
                            }
                        } else if constexpr (std::is_same_v<T, std::monostate>) {
                            if (4 + offsets.size() * 2 + data.size() + (num_rows / 8 + 1)
                                > PAGE_SIZE) {
                                save_page();
                            }
                            unset_bitmap(bitmap, num_rows);
                            ++num_rows;
                        } else {
                            throw std::runtime_error("not string or null");
                        }
                    },
                    value);
            }
            if (num_rows != 0) {
                save_page();
            }
            break;
        }
        }
    }
    return ret;
}
