/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Platform.h>
#include <AK/ScopeGuard.h>
#include <LibCore/DeprecatedFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#ifdef AK_OS_SERENITY
#    include <serenity.h>
#endif

// On Linux distros that use glibc `basename` is defined as a macro that expands to `__xpg_basename`, so we undefine it
#if defined(AK_OS_LINUX) && defined(basename)
#    undef basename
#endif

namespace Core {

ErrorOr<NonnullRefPtr<DeprecatedFile>> DeprecatedFile::open(DeprecatedString filename, OpenMode mode, mode_t permissions)
{
    auto file = DeprecatedFile::construct(move(filename));
    if (!file->open_impl(mode, permissions))
        return Error::from_errno(file->error());
    return file;
}

DeprecatedFile::DeprecatedFile(DeprecatedString filename, Object* parent)
    : IODevice(parent)
    , m_filename(move(filename))
{
}

DeprecatedFile::~DeprecatedFile()
{
    if (m_should_close_file_descriptor == ShouldCloseFileDescriptor::Yes && mode() != OpenMode::NotOpen)
        close();
}

bool DeprecatedFile::open(int fd, OpenMode mode, ShouldCloseFileDescriptor should_close)
{
    set_fd(fd);
    set_mode(mode);
    m_should_close_file_descriptor = should_close;
    return true;
}

bool DeprecatedFile::open(OpenMode mode)
{
    return open_impl(mode, 0666);
}

bool DeprecatedFile::open_impl(OpenMode mode, mode_t permissions)
{
    VERIFY(!m_filename.is_null());
    int flags = 0;
    if (has_flag(mode, OpenMode::ReadOnly) && has_flag(mode, OpenMode::WriteOnly)) {
        flags |= O_RDWR | O_CREAT;
    } else if (has_flag(mode, OpenMode::ReadOnly)) {
        flags |= O_RDONLY;
    } else if (has_flag(mode, OpenMode::WriteOnly)) {
        flags |= O_WRONLY | O_CREAT;
        bool should_truncate = !(has_flag(mode, OpenMode::Append) || has_flag(mode, OpenMode::MustBeNew));
        if (should_truncate)
            flags |= O_TRUNC;
    }
    if (has_flag(mode, OpenMode::Append))
        flags |= O_APPEND;
    if (has_flag(mode, OpenMode::Truncate))
        flags |= O_TRUNC;
    if (has_flag(mode, OpenMode::MustBeNew))
        flags |= O_EXCL;
    if (!has_flag(mode, OpenMode::KeepOnExec))
        flags |= O_CLOEXEC;
    int fd = ::open(m_filename.characters(), flags, permissions);
    if (fd < 0) {
        set_error(errno);
        return false;
    }

    set_fd(fd);
    set_mode(mode);
    return true;
}

int DeprecatedFile::leak_fd()
{
    m_should_close_file_descriptor = ShouldCloseFileDescriptor::No;
    return fd();
}

bool DeprecatedFile::is_device() const
{
    struct stat st;
    if (fstat(fd(), &st) < 0)
        return false;
    return S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode);
}

bool DeprecatedFile::is_block_device() const
{
    struct stat stat;
    if (fstat(fd(), &stat) < 0)
        return false;
    return S_ISBLK(stat.st_mode);
}

bool DeprecatedFile::is_char_device() const
{
    struct stat stat;
    if (fstat(fd(), &stat) < 0)
        return false;
    return S_ISCHR(stat.st_mode);
}

bool DeprecatedFile::is_directory() const
{
    struct stat st;
    if (fstat(fd(), &st) < 0)
        return false;
    return S_ISDIR(st.st_mode);
}

bool DeprecatedFile::is_link() const
{
    struct stat stat;
    if (fstat(fd(), &stat) < 0)
        return false;
    return S_ISLNK(stat.st_mode);
}

DeprecatedString DeprecatedFile::real_path_for(DeprecatedString const& filename)
{
    if (filename.is_null())
        return {};
    auto* path = realpath(filename.characters(), nullptr);
    DeprecatedString real_path(path);
    free(path);
    return real_path;
}

DeprecatedString DeprecatedFile::current_working_directory()
{
    char* cwd = getcwd(nullptr, 0);
    if (!cwd) {
        perror("getcwd");
        return {};
    }

    auto cwd_as_string = DeprecatedString(cwd);
    free(cwd);

    return cwd_as_string;
}

DeprecatedString DeprecatedFile::absolute_path(DeprecatedString const& path)
{
    if (!Core::System::stat(path).is_error())
        return DeprecatedFile::real_path_for(path);

    if (path.starts_with("/"sv))
        return LexicalPath::canonicalized_path(path);

    auto working_directory = DeprecatedFile::current_working_directory();
    auto full_path = LexicalPath::join(working_directory, path);

    return LexicalPath::canonicalized_path(full_path.string());
}

static DeprecatedString get_duplicate_name(DeprecatedString const& path, int duplicate_count)
{
    if (duplicate_count == 0) {
        return path;
    }
    LexicalPath lexical_path(path);
    StringBuilder duplicated_name;
    duplicated_name.append('/');
    auto& parts = lexical_path.parts_view();
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        duplicated_name.appendff("{}/", parts[i]);
    }
    auto prev_duplicate_tag = DeprecatedString::formatted("({})", duplicate_count);
    auto title = lexical_path.title();
    if (title.ends_with(prev_duplicate_tag)) {
        // remove the previous duplicate tag "(n)" so we can add a new tag.
        title = title.substring_view(0, title.length() - prev_duplicate_tag.length());
    }
    duplicated_name.appendff("{} ({})", title, duplicate_count);
    if (!lexical_path.extension().is_empty()) {
        duplicated_name.appendff(".{}", lexical_path.extension());
    }
    return duplicated_name.to_deprecated_string();
}

ErrorOr<void, DeprecatedFile::CopyError> DeprecatedFile::copy_file_or_directory(DeprecatedString const& dst_path, DeprecatedString const& src_path, RecursionMode recursion_mode, LinkMode link_mode, AddDuplicateFileMarker add_duplicate_file_marker, PreserveMode preserve_mode)
{
    if (add_duplicate_file_marker == AddDuplicateFileMarker::Yes) {
        int duplicate_count = 0;
        while (access(get_duplicate_name(dst_path, duplicate_count).characters(), F_OK) == 0) {
            ++duplicate_count;
        }
        if (duplicate_count != 0) {
            return copy_file_or_directory(get_duplicate_name(dst_path, duplicate_count), src_path, RecursionMode::Allowed, LinkMode::Disallowed, AddDuplicateFileMarker::Yes, preserve_mode);
        }
    }

    auto source_or_error = DeprecatedFile::open(src_path, OpenMode::ReadOnly);
    if (source_or_error.is_error())
        return CopyError { errno, false };

    auto& source = *source_or_error.value();

    struct stat src_stat;
    if (fstat(source.fd(), &src_stat) < 0)
        return CopyError { errno, false };

    if (source.is_directory()) {
        if (recursion_mode == RecursionMode::Disallowed)
            return CopyError { errno, true };
        return copy_directory(dst_path, src_path, src_stat);
    }

    if (link_mode == LinkMode::Allowed) {
        if (link(src_path.characters(), dst_path.characters()) < 0)
            return CopyError { errno, false };

        return {};
    }

    return copy_file(dst_path, src_stat, source, preserve_mode);
}

ErrorOr<void, DeprecatedFile::CopyError> DeprecatedFile::copy_file(DeprecatedString const& dst_path, struct stat const& src_stat, DeprecatedFile& source, PreserveMode preserve_mode)
{
    int dst_fd = creat(dst_path.characters(), 0666);
    if (dst_fd < 0) {
        if (errno != EISDIR)
            return CopyError { errno, false };

        auto dst_dir_path = DeprecatedString::formatted("{}/{}", dst_path, LexicalPath::basename(source.filename()));
        dst_fd = creat(dst_dir_path.characters(), 0666);
        if (dst_fd < 0)
            return CopyError { errno, false };
    }

    ScopeGuard close_fd_guard([dst_fd]() { ::close(dst_fd); });

    if (src_stat.st_size > 0) {
        if (ftruncate(dst_fd, src_stat.st_size) < 0)
            return CopyError { errno, false };
    }

    for (;;) {
        char buffer[32768];
        ssize_t nread = ::read(source.fd(), buffer, sizeof(buffer));
        if (nread < 0) {
            return CopyError { errno, false };
        }
        if (nread == 0)
            break;
        ssize_t remaining_to_write = nread;
        char* bufptr = buffer;
        while (remaining_to_write) {
            ssize_t nwritten = ::write(dst_fd, bufptr, remaining_to_write);
            if (nwritten < 0)
                return CopyError { errno, false };

            VERIFY(nwritten > 0);
            remaining_to_write -= nwritten;
            bufptr += nwritten;
        }
    }

    auto my_umask = umask(0);
    umask(my_umask);
    // NOTE: We don't copy the set-uid and set-gid bits unless requested.
    if (!has_flag(preserve_mode, PreserveMode::Permissions))
        my_umask |= 06000;

    if (fchmod(dst_fd, src_stat.st_mode & ~my_umask) < 0)
        return CopyError { errno, false };

    if (has_flag(preserve_mode, PreserveMode::Ownership)) {
        if (fchown(dst_fd, src_stat.st_uid, src_stat.st_gid) < 0)
            return CopyError { errno, false };
    }

    if (has_flag(preserve_mode, PreserveMode::Timestamps)) {
        struct timespec times[2] = {
#ifdef AK_OS_MACOS
            src_stat.st_atimespec,
            src_stat.st_mtimespec,
#else
            src_stat.st_atim,
            src_stat.st_mtim,
#endif
        };
        if (utimensat(AT_FDCWD, dst_path.characters(), times, 0) < 0)
            return CopyError { errno, false };
    }

    return {};
}

ErrorOr<void, DeprecatedFile::CopyError> DeprecatedFile::copy_directory(DeprecatedString const& dst_path, DeprecatedString const& src_path, struct stat const& src_stat, LinkMode link, PreserveMode preserve_mode)
{
    if (mkdir(dst_path.characters(), 0755) < 0)
        return CopyError { errno, false };

    DeprecatedString src_rp = DeprecatedFile::real_path_for(src_path);
    src_rp = DeprecatedString::formatted("{}/", src_rp);
    DeprecatedString dst_rp = DeprecatedFile::real_path_for(dst_path);
    dst_rp = DeprecatedString::formatted("{}/", dst_rp);

    if (!dst_rp.is_empty() && dst_rp.starts_with(src_rp))
        return CopyError { errno, false };

    DirIterator di(src_path, DirIterator::SkipParentAndBaseDir);
    if (di.has_error())
        return CopyError { errno, false };

    while (di.has_next()) {
        DeprecatedString filename = di.next_path();
        auto result = copy_file_or_directory(
            DeprecatedString::formatted("{}/{}", dst_path, filename),
            DeprecatedString::formatted("{}/{}", src_path, filename),
            RecursionMode::Allowed, link, AddDuplicateFileMarker::Yes, preserve_mode);
        if (result.is_error())
            return result.release_error();
    }

    auto my_umask = umask(0);
    umask(my_umask);

    if (chmod(dst_path.characters(), src_stat.st_mode & ~my_umask) < 0)
        return CopyError { errno, false };

    if (has_flag(preserve_mode, PreserveMode::Ownership)) {
        if (chown(dst_path.characters(), src_stat.st_uid, src_stat.st_gid) < 0)
            return CopyError { errno, false };
    }

    if (has_flag(preserve_mode, PreserveMode::Timestamps)) {
        struct timespec times[2] = {
#ifdef AK_OS_MACOS
            src_stat.st_atimespec,
            src_stat.st_mtimespec,
#else
            src_stat.st_atim,
            src_stat.st_mtim,
#endif
        };
        if (utimensat(AT_FDCWD, dst_path.characters(), times, 0) < 0)
            return CopyError { errno, false };
    }

    return {};
}

Optional<DeprecatedString> DeprecatedFile::resolve_executable_from_environment(StringView filename)
{
    if (filename.is_empty())
        return {};

    // Paths that aren't just a file name generally count as already resolved.
    if (filename.contains('/')) {
        if (access(DeprecatedString { filename }.characters(), X_OK) != 0)
            return {};

        return filename;
    }

    auto const* path_str = getenv("PATH");
    StringView path;
    if (path_str)
        path = { path_str, strlen(path_str) };
    if (path.is_empty())
        path = DEFAULT_PATH_SV;

    auto directories = path.split_view(':');

    for (auto directory : directories) {
        auto file = DeprecatedString::formatted("{}/{}", directory, filename);

        if (access(file.characters(), X_OK) == 0)
            return file;
    }

    return {};
};

}
