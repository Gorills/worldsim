#include "worldsim/c_api.h"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {
namespace fs=std::filesystem;

void check(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<char> read_bytes(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    check(static_cast<bool>(file),"cannot read snapshot test file");
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}

struct TestDirectory {
    fs::path path;
    TestDirectory() {
        for (unsigned i=0;i<128U;++i) {
            path=fs::current_path()/("snapshot-file-tests-"+std::to_string(i));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("cannot reserve snapshot test directory");
    }
    ~TestDirectory() {
        std::error_code ignored;
        fs::remove_all(path,ignored);
    }
};

#if defined(__unix__) || defined(__APPLE__)
class FileSizeLimit {
public:
    explicit FileSizeLimit(rlim_t bytes=1024) {
        check(getrlimit(RLIMIT_FSIZE,&saved_)==0,"getrlimit failed");
        previous_signal_=std::signal(SIGXFSZ,SIG_IGN);
        check(previous_signal_!=SIG_ERR,"cannot ignore SIGXFSZ");
        rlimit limited=saved_;
        limited.rlim_cur=bytes;
        if (setrlimit(RLIMIT_FSIZE,&limited)!=0) {
            std::signal(SIGXFSZ,previous_signal_);
            throw std::runtime_error("setrlimit failed");
        }
    }
    ~FileSizeLimit() {
        (void)setrlimit(RLIMIT_FSIZE,&saved_);
        std::signal(SIGXFSZ,previous_signal_);
    }
private:
    rlimit saved_{};
    using SignalHandler=void (*)(int);
    SignalHandler previous_signal_{};
};
#endif

void snapshot_file_transactions() {
    TestDirectory directory;
    // Even a filename resembling a staging prefix must be a normal save slot.
    const fs::path path=directory.path/".worldsim-save-0";
    const std::string filename=path.string();
    std::unique_ptr<ws_handle,decltype(&ws_destroy)> handle(ws_create_default(42),ws_destroy);
    check(handle!=nullptr,"cannot create snapshot test world");
    // Stale names (files or directories) are not owned by this save.
    const fs::path stale_file=filename+".tmp-0";
    const fs::path stale_directory=filename+".tmp-1";
    { std::ofstream file(stale_file); file << "not ours"; }
    const auto stale_bytes=read_bytes(stale_file);
    fs::create_directory(stale_directory);
    check(ws_save_snapshot_file(handle.get(),filename.c_str())==1,"initial snapshot save failed");
    check(read_bytes(stale_file)==stale_bytes,"save damaged a stale temporary file");
    check(fs::is_directory(stale_directory),"save removed a stale temporary directory");
    fs::remove(stale_file);
    fs::remove(stale_directory);
#if defined(__unix__) || defined(__APPLE__)
    fs::permissions(path,fs::perms::owner_read|fs::perms::owner_write);
#endif
    const auto original_permissions=fs::status(path).permissions();
    const auto original=read_bytes(path);
    check(original.size()>1024U,"snapshot too small for partial-write regression");
    check(ws_step(handle.get(),1)==1,"snapshot test step failed");

#if defined(__unix__) || defined(__APPLE__)
    const std::string candidate=(directory.path/"candidate.snapshot").string();
    check(ws_save_snapshot_file(handle.get(),candidate.c_str())==1,"candidate snapshot save failed");
    const auto candidate_size=read_bytes(candidate).size();
    fs::remove(candidate);
    int saved=1;
    // Exercise an early partial write and a near-complete write that can fail
    // only when the final buffered bytes are flushed at close.
    for (rlim_t bytes:{rlim_t{1024},static_cast<rlim_t>(candidate_size-1U)}) {
        {
            // The process limit and signal handler are restored before checks.
            FileSizeLimit limit(bytes);
            saved=ws_save_snapshot_file(handle.get(),filename.c_str());
        }
        check(saved==0,"partial snapshot write reported success");
        const std::string error=ws_last_error(handle.get());
        check(!error.empty(),"failed write has no diagnostic");
        check(read_bytes(path)==original,"failed save destroyed previous snapshot");
        check(ws_tick(handle.get())==1U,"failed save changed live simulation");
        check(std::distance(fs::directory_iterator(directory.path),fs::directory_iterator())==1,
              "failed save leaked staging files");
        std::cout << "rejected limited write: " << error << '\n';
    }

    const fs::path absent=directory.path/"absent.snapshot";
    const std::string absent_name=absent.string();
    {
        FileSizeLimit limit;
        saved=ws_save_snapshot_file(handle.get(),absent_name.c_str());
    }
    check(saved==0 && !fs::exists(absent),"failed first save published partial snapshot");
    check(std::distance(fs::directory_iterator(directory.path),fs::directory_iterator())==1,
          "failed first save leaked staging files");
#else
    std::cout << "RLIMIT_FSIZE partial-write injection unavailable on this platform\n";
#endif

    check(ws_load_snapshot_file(handle.get(),filename.c_str())==1,"previous save cannot be reloaded");
    check(ws_tick(handle.get())==0U,"previous save tick changed");
    check(ws_step(handle.get(),2)==1,"replacement snapshot step failed");
    check(ws_save_snapshot_file(handle.get(),filename.c_str())==1,"snapshot replacement failed");
    const auto updated=read_bytes(path);
    check(fs::status(path).permissions()==original_permissions,"replacement changed file permissions");
    check(updated!=original,"replacement did not publish new world");
    check(ws_step(handle.get(),1)==1,"post-save step failed");
    check(ws_load_snapshot_file(handle.get(),filename.c_str())==1,"replacement cannot be loaded");
    check(ws_tick(handle.get())==2U,"replacement did not restore saved tick");
    const std::string roundtrip=(directory.path/"roundtrip.snapshot").string();
    check(ws_save_snapshot_file(handle.get(),roundtrip.c_str())==1,"round-trip save failed");
    check(read_bytes(roundtrip)==updated,"file snapshot round-trip changed bytes");
    fs::remove(roundtrip);

#if defined(__unix__) || defined(__APPLE__)
    const fs::path link=directory.path/"linked.snapshot";
    fs::create_symlink(path,link);
    check(ws_step(handle.get(),1)==1,"linked save step failed");
    const std::string link_name=link.string();
    check(ws_save_snapshot_file(handle.get(),link_name.c_str())==1,"linked slot save failed");
    check(!fs::is_symlink(link),"save wrote through the destination link");
    check(read_bytes(path)==updated,"linked save changed its previous target");
    check(ws_load_snapshot_file(handle.get(),link_name.c_str())==1,"linked slot cannot be loaded");
    check(ws_tick(handle.get())==3U,"linked slot did not publish new state");
    fs::remove(link);
#endif

    // A publication error must not delete an existing destination directory.
    const fs::path blocked=directory.path/"blocked";
    fs::create_directory(blocked);
    const fs::path marker=blocked/"keep";
    { std::ofstream file(marker); file << "keep"; }
    const auto marker_bytes=read_bytes(marker);
    const std::string blocked_name=blocked.string();
    check(ws_save_snapshot_file(handle.get(),blocked_name.c_str())==0,"save replaced a directory");
    check(read_bytes(marker)==marker_bytes,"publication failure damaged destination");
    check(read_bytes(path)==updated,"publication failure changed another save");
    check(std::distance(fs::directory_iterator(directory.path),fs::directory_iterator())==2,
          "publication failure leaked staging files");

    const std::string missing=(directory.path/"missing"/"world.snapshot").string();
    check(ws_save_snapshot_file(handle.get(),missing.c_str())==0,"save accepted missing parent");
    check(!fs::exists(directory.path/"missing"),"save unexpectedly created parent directory");
    check(ws_save_snapshot_file(handle.get(),nullptr)==0,"save accepted null path");
    check(ws_save_snapshot_file(handle.get(),"")==0,"save accepted empty path");
}

} // namespace

int main() {
    try {
        snapshot_file_transactions();
        std::cout << "snapshot file tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "snapshot file test failure: " << error.what() << '\n';
        return 1;
    }
}
