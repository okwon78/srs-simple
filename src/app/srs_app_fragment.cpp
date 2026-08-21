// srs_simple — 원본: trunk/src/app/srs_app_fragment.cpp
#include <srs_app_fragment.hpp>

#include <stdio.h>
#include <unistd.h>

#include <srs_kernel_error.hpp>
#include <srs_kernel_file.hpp>
#include <srs_kernel_log.hpp>

using namespace std;

#define srs_min(a, b) (((a) < (b))? (a) : (b))
#define srs_max(a, b) (((a) < (b))? (b) : (a))

SrsFragment::SrsFragment()
{
    dur = 0;
    start_dts = -1;
    sequence_header = false;
}

SrsFragment::~SrsFragment()
{
}

void SrsFragment::append(int64_t dts)
{
    // The dts in ms to srs_utime_t.
    srs_utime_t dts_in_tbn = dts * SRS_UTIME_MILLISECONDS;

    if (start_dts == -1) {
        start_dts = dts_in_tbn;
    }

    // 세그먼트 안에서 가장 이른 dts를 기준으로 델타를 잰다
    // (오디오/비디오가 인터리브되어 dts가 앞뒤로 흔들릴 수 있다).
    start_dts = srs_min(start_dts, dts_in_tbn);
    dur = dts_in_tbn - start_dts;
}

srs_utime_t SrsFragment::duration()
{
    return dur;
}

bool SrsFragment::is_sequence_header()
{
    return sequence_header;
}

void SrsFragment::set_sequence_header(bool v)
{
    sequence_header = v;
}

string SrsFragment::fullpath()
{
    return filepath;
}

void SrsFragment::set_path(string v)
{
    filepath = v;
}

string SrsFragment::tmppath()
{
    return filepath + ".tmp";
}

srs_error_t SrsFragment::create_dir()
{
    srs_error_t err = srs_success;

    size_t pos = filepath.rfind('/');
    if (pos == string::npos) {
        return err;
    }

    string dir = filepath.substr(0, pos);
    if (dir.empty() || srs_path_exists(dir)) {
        return err;
    }

    if ((err = srs_create_dir_recursively(dir)) != srs_success) {
        return srs_error_wrap(err, "create dir %s", dir.c_str());
    }

    return err;
}

srs_error_t SrsFragment::rename()
{
    srs_error_t err = srs_success;

    string full_path = fullpath();
    string tmp_file = tmppath();

    if (::rename(tmp_file.c_str(), full_path.c_str()) < 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_RENAME, "rename %s to %s", tmp_file.c_str(), full_path.c_str());
    }

    return err;
}

srs_error_t SrsFragment::unlink_file()
{
    srs_error_t err = srs_success;

    if (::unlink(filepath.c_str()) < 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_WRITE, "unlink %s", filepath.c_str());
    }

    return err;
}

srs_error_t SrsFragment::unlink_tmpfile()
{
    srs_error_t err = srs_success;

    string tmp_file = tmppath();
    if (srs_path_exists(tmp_file) && ::unlink(tmp_file.c_str()) < 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_WRITE, "unlink tmp %s", tmp_file.c_str());
    }

    return err;
}

SrsFragmentWindow::SrsFragmentWindow()
{
}

SrsFragmentWindow::~SrsFragmentWindow()
{
    vector<SrsFragment*>::iterator it;
    for (it = fragments.begin(); it != fragments.end(); ++it) {
        SrsFragment* fragment = *it;
        srs_freep(fragment);
    }
    fragments.clear();

    for (it = expired_fragments.begin(); it != expired_fragments.end(); ++it) {
        SrsFragment* fragment = *it;
        srs_freep(fragment);
    }
    expired_fragments.clear();
}

void SrsFragmentWindow::append(SrsFragment* fragment)
{
    fragments.push_back(fragment);
}

void SrsFragmentWindow::shrink(srs_utime_t window)
{
    srs_utime_t duration = 0;

    int remove_index = -1;

    // 최신(뒤)에서 과거(앞)로 누적 — 윈도우를 넘긴 지점 앞은 전부 만료.
    for (int i = (int)fragments.size() - 1; i >= 0; i--) {
        SrsFragment* fragment = fragments[i];
        duration += fragment->duration();

        if (duration > window) {
            remove_index = i;
            break;
        }
    }

    for (int i = 0; i < remove_index && !fragments.empty(); i++) {
        SrsFragment* fragment = *fragments.begin();
        fragments.erase(fragments.begin());
        expired_fragments.push_back(fragment);
    }
}

void SrsFragmentWindow::clear_expired(bool delete_files)
{
    srs_error_t err = srs_success;

    vector<SrsFragment*>::iterator it;
    for (it = expired_fragments.begin(); it != expired_fragments.end(); ++it) {
        SrsFragment* fragment = *it;
        if (delete_files && (err = fragment->unlink_file()) != srs_success) {
            srs_warn("Unlink ts failed, %s", srs_error_desc(err).c_str());
            srs_freep(err);
        }
        srs_freep(fragment);
    }

    expired_fragments.clear();
}

srs_utime_t SrsFragmentWindow::max_duration()
{
    srs_utime_t v = 0;

    vector<SrsFragment*>::iterator it;
    for (it = fragments.begin(); it != fragments.end(); ++it) {
        SrsFragment* fragment = *it;
        v = srs_max(v, fragment->duration());
    }

    return v;
}

bool SrsFragmentWindow::empty()
{
    return fragments.empty();
}

SrsFragment* SrsFragmentWindow::first()
{
    return fragments.at(0);
}

int SrsFragmentWindow::size()
{
    return (int)fragments.size();
}

SrsFragment* SrsFragmentWindow::at(int index)
{
    return fragments.at(index);
}
