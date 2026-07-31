#pragma once
// Dependency injection layer for OS process operations.
// Enables unit-testing of injector logic by swapping in mock implementations.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

struct IProcessOps {
    virtual ~IProcessOps() = default;

    // Process handle
    virtual bool open_process(int pid, void*& out_handle) = 0;
    virtual void close_handle(void* handle) = 0;

    // Memory allocation in target
    virtual bool alloc_mem(void* handle, size_t size, void*& out_addr) = 0;
    virtual bool write_mem(void* handle, void* addr, const void* data, size_t size) = 0;
    virtual bool free_mem(void* handle, void* addr) = 0;

    // Thread creation
    virtual bool create_remote_thread(void* handle, void* start_addr, void* arg,
                                      void*& out_thread) = 0;
    virtual bool wait_for_thread(void* thread, uint32_t timeout_ms) = 0;
    virtual bool get_thread_exit_code(void* thread, uint32_t& out_code) = 0;
    virtual void terminate_thread(void* thread) = 0;
    virtual void close_thread(void* thread) = 0;

    // Module enumeration
    virtual bool enum_modules(void* handle, std::vector<std::string>& out_names) = 0;
    virtual bool get_module_handle(void*& out_handle, const std::string& name) = 0;
    virtual bool get_proc_address(void* mod, const std::string& name,
                                  void*& out_addr) = 0;

    // DLL loading/unloading address (kernel32)
    virtual bool get_load_library_addr(void*& out_addr) = 0;

    // File I/O for PE parsing
    virtual bool read_file_bytes(const fs::path& path, std::vector<uint8_t>& out) = 0;

    // CSPRNG
    virtual bool generate_random(uint8_t* buf, size_t len) = 0;

    // Last error
    virtual std::string last_error() = 0;
};

#ifdef _WIN32
// Real implementation — delegates to actual Windows APIs.
struct Win32ProcessOps : IProcessOps {
    bool open_process(int pid, void*& out_handle) override;
    void close_handle(void* handle) override;
    bool alloc_mem(void* handle, size_t size, void*& out_addr) override;
    bool write_mem(void* handle, void* addr, const void* data, size_t size) override;
    bool free_mem(void* handle, void* addr) override;
    bool create_remote_thread(void* handle, void* start_addr, void* arg,
                              void*& out_thread) override;
    bool wait_for_thread(void* thread, uint32_t timeout_ms) override;
    bool get_thread_exit_code(void* thread, uint32_t& out_code) override;
    void terminate_thread(void* thread) override;
    void close_thread(void* thread) override;
    bool enum_modules(void* handle, std::vector<std::string>& out_names) override;
    bool get_module_handle(void*& out_handle, const std::string& name) override;
    bool get_proc_address(void* mod, const std::string& name, void*& out_addr) override;
    bool get_load_library_addr(void*& out_addr) override;
    bool read_file_bytes(const fs::path& path, std::vector<uint8_t>& out) override;
    bool generate_random(uint8_t* buf, size_t len) override;
    std::string last_error() override;
};
#endif

// Test mock — configurable return values for every operation.
struct MockProcessOps : IProcessOps {
    // Configurable results
    bool open_process_result = true;
    void* open_process_handle = reinterpret_cast<void*>(1);
    bool alloc_mem_result = true;
    void* alloc_mem_addr = reinterpret_cast<void*>(0x10000000);
    bool write_mem_result = true;
    bool free_mem_result = true;
    bool create_thread_result = true;
    void* create_thread_handle = reinterpret_cast<void*>(10);
    bool wait_result = true;    // true = WAIT_OBJECT_0, false = timeout
    uint32_t thread_exit_code = 1;  // non-zero = DLL base for LoadLibraryW
    std::vector<std::string> module_names;
    void* module_handle = reinterpret_cast<void*>(0x20000000);
    void* proc_address = reinterpret_cast<void*>(0x30000000);
    void* load_lib_addr = reinterpret_cast<void*>(0x40000000);
    std::vector<uint8_t> file_bytes;
    bool read_file_result = true;
    bool random_result = true;
    std::string last_error_str;
    std::vector<uint8_t> random_bytes;

    // Call record for assertions
    struct CallRecord {
        std::string name;
        int pid = 0;
        size_t size = 0;
        std::string module_name;
    };
    mutable std::vector<CallRecord> calls;

    bool open_process(int pid, void*& out_handle) override {
        calls.push_back({"open_process", pid, 0, ""});
        out_handle = open_process_handle;
        return open_process_result;
    }
    void close_handle(void*) override {}
    bool alloc_mem(void* h, size_t size, void*& out_addr) override {
        (void)h;
        calls.push_back({"alloc_mem", 0, size, ""});
        out_addr = alloc_mem_addr;
        return alloc_mem_result;
    }
    bool write_mem(void* h, void* addr, const void* data, size_t size) override {
        (void)h; (void)addr; (void)data;
        calls.push_back({"write_mem", 0, size, ""});
        return write_mem_result;
    }
    bool free_mem(void*, void*) override { return free_mem_result; }
    bool create_remote_thread(void* h, void* start, void* arg,
                              void*& out_thread) override {
        (void)h; (void)start; (void)arg;
        calls.push_back({"create_thread", 0, 0, ""});
        out_thread = create_thread_handle;
        return create_thread_result;
    }
    bool wait_for_thread(void*, uint32_t) override { return wait_result; }
    bool get_thread_exit_code(void*, uint32_t& out_code) override {
        out_code = thread_exit_code;
        return true;
    }
    void terminate_thread(void*) override {}
    void close_thread(void*) override {}
    bool enum_modules(void* h, std::vector<std::string>& out_names) override {
        (void)h;
        calls.push_back({"enum_modules", 0, 0, ""});
        out_names = module_names;
        return true;
    }
    bool get_module_handle(void*& out_handle, const std::string& name) override {
        calls.push_back({"get_module", 0, 0, name});
        out_handle = module_handle;
        return true;
    }
    bool get_proc_address(void*, const std::string& name, void*& out_addr) override {
        calls.push_back({"get_proc", 0, 0, name});
        out_addr = proc_address;
        return true;
    }
    bool get_load_library_addr(void*& out_addr) override {
        out_addr = load_lib_addr;
        return true;
    }
    bool read_file_bytes(const fs::path&, std::vector<uint8_t>& out) override {
        out = file_bytes;
        return read_file_result;
    }
    bool generate_random(uint8_t* buf, size_t len) override {
        if (random_result) {
            for (size_t i = 0; i < len && i < random_bytes.size(); ++i)
                buf[i] = random_bytes[i];
            return true;
        }
        return false;
    }
    std::string last_error() override { return last_error_str; }
};
