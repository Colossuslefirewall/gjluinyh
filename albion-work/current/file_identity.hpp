#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <stdexcept>
namespace albion {
struct FileVersion {
    std::uint64_t bytes=0, written=0, created=0, identity=0;
    bool operator==(const FileVersion&) const = default;
};
inline FileVersion FileVersionOf(const BY_HANDLE_FILE_INFORMATION& info) {
    return {(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow,
        (std::uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime,
        (std::uint64_t(info.ftCreationTime.dwHighDateTime)<<32)|info.ftCreationTime.dwLowDateTime,
        ((std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow)^info.dwVolumeSerialNumber};
}
class ModelReadLease {
    HANDLE file_=INVALID_HANDLE_VALUE;
public:
    FileVersion version;
    explicit ModelReadLease(const std::wstring& path) {
        file_=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file_==INVALID_HANDLE_VALUE) throw std::runtime_error("Model is missing, locked or inaccessible (Windows error "+std::to_string(GetLastError())+")");
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(file_,&info)) { Close();throw std::runtime_error("Cannot inspect model source"); }
        version=FileVersionOf(info);
    }
    ~ModelReadLease(){Close();}
    ModelReadLease(const ModelReadLease&)=delete;
    ModelReadLease& operator=(const ModelReadLease&)=delete;
    void Close(){if(file_!=INVALID_HANDLE_VALUE){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;}}
};
}
