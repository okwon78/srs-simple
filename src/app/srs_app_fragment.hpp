// srs_simple — 원본: trunk/src/app/srs_app_fragment.hpp
// HLS 세그먼트의 공통 수명주기:
//   - 쓰는 동안은 .tmp 파일 → 완성되면 rename (플레이어가 미완성 세그먼트를 받지 않게)
//   - duration은 세그먼트에 들어간 프레임 dts의 (최대-최소) 델타
//   - SrsFragmentWindow가 살아있는 세그먼트 목록을 유지하고, hls_window를 넘긴
//     오래된 세그먼트를 만료 → 파일 삭제한다 (라이브 롤링 윈도우)
// 원본 대비 제거: DVR 공유 경로, snapshot — CLAUDE.md §5.6 S10.
#ifndef SRS_APP_FRAGMENT_HPP
#define SRS_APP_FRAGMENT_HPP

#include <srs_core.hpp>

#include <string>
#include <vector>

// The fragment is a media file: HLS의 ts 세그먼트 1개.
class SrsFragment
{
private:
    // The duration in srs_utime_t.
    srs_utime_t dur;
    // The start dts in srs_utime_t, -1 until first append.
    srs_utime_t start_dts;
    // Whether current fragment contains sequence header (m3u8의 EXT-X-DISCONTINUITY 판단).
    bool sequence_header;
    // The full file path of fragment.
    std::string filepath;
public:
    SrsFragment();
    virtual ~SrsFragment();
public:
    // Append a frame with dts into fragment, to update the duration.
    // @param dts The dts of frame in ms.
    virtual void append(int64_t dts);
    // Get the duration of fragment in srs_utime_t.
    virtual srs_utime_t duration();
    // Whether the fragment contains any sequence header.
    virtual bool is_sequence_header();
    // Set whether contains sequence header.
    virtual void set_sequence_header(bool v);
    // Get the full path of fragment.
    virtual std::string fullpath();
    // Set the full path of fragment.
    virtual void set_path(std::string v);
    // Get the temporary path for file to write (fullpath + ".tmp").
    virtual std::string tmppath();
    // Create the dir for file recursively.
    virtual srs_error_t create_dir();
public:
    // Rename the temporary file to fullpath, when fragment is complete.
    virtual srs_error_t rename();
    // Unlink the fragment file (만료된 세그먼트 삭제).
    virtual srs_error_t unlink_file();
    // Unlink the temporary file (비정상 종료한 세그먼트 정리).
    virtual srs_error_t unlink_tmpfile();
};

// The fragment window manage a series of fragments:
// 살아있는 세그먼트 목록 = m3u8에 실리는 목록. shrink가 롤링 윈도우를 유지한다.
class SrsFragmentWindow
{
private:
    std::vector<SrsFragment*> fragments;
    // The expired fragments, need to be removed then freed.
    std::vector<SrsFragment*> expired_fragments;
public:
    SrsFragmentWindow();
    virtual ~SrsFragmentWindow();
public:
    // Append a new fragment, which is ready to delivery to client.
    virtual void append(SrsFragment* fragment);
    // Shrink the window, move deprecated fragments to expired.
    // @param window The duration in srs_utime_t to keep.
    virtual void shrink(srs_utime_t window);
    // Clear the expired fragments.
    // @param delete_files Whether unlink the fragment files.
    virtual void clear_expired(bool delete_files);
    // Get the max duration in srs_utime_t of all fragments.
    virtual srs_utime_t max_duration();
public:
    virtual bool empty();
    virtual SrsFragment* first();
    virtual int size();
    virtual SrsFragment* at(int index);
};

#endif
