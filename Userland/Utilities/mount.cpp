/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Stream.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int parse_options(StringView options)
{
    int flags = 0;
    Vector<StringView> parts = options.split_view(',');
    for (auto& part : parts) {
        if (part == "defaults")
            continue;
        else if (part == "nodev")
            flags |= MS_NODEV;
        else if (part == "noexec")
            flags |= MS_NOEXEC;
        else if (part == "nosuid")
            flags |= MS_NOSUID;
        else if (part == "bind")
            flags |= MS_BIND;
        else if (part == "ro")
            flags |= MS_RDONLY;
        else if (part == "remount")
            flags |= MS_REMOUNT;
        else if (part == "wxallowed")
            flags |= MS_WXALLOWED;
        else if (part == "axallowed")
            flags |= MS_AXALLOWED;
        else if (part == "noregular")
            flags |= MS_NOREGULAR;
        else
            warnln("Ignoring invalid option: {}", part);
    }
    return flags;
}

static bool is_source_none(StringView source)
{
    return source == "none"sv;
}

static ErrorOr<int> get_source_fd(StringView source)
{
    if (is_source_none(source))
        return -1;
    auto fd_or_error = Core::System::open(source, O_RDWR);
    if (fd_or_error.is_error())
        fd_or_error = Core::System::open(source, O_RDONLY);
    return fd_or_error;
}

static bool mount_by_line(String const& line)
{
    // Skip comments and blank lines.
    if (line.is_empty() || line.starts_with('#'))
        return true;

    Vector<String> parts = line.split('\t');
    if (parts.size() < 3) {
        warnln("Invalid fstab entry: {}", line);
        return false;
    }

    auto mountpoint = parts[1];
    auto fstype = parts[2];
    int flags = parts.size() >= 4 ? parse_options(parts[3]) : 0;

    if (mountpoint == "/") {
        dbgln("Skipping mounting root");
        return true;
    }

    auto filename = parts[0];

    auto fd_or_error = get_source_fd(filename);
    if (fd_or_error.is_error()) {
        outln("{}", fd_or_error.release_error());
        return false;
    }
    auto const fd = fd_or_error.release_value();

    dbgln("Mounting {} ({}) on {}", filename, fstype, mountpoint);

    auto error_or_void = Core::System::mount(fd, mountpoint, fstype, flags);
    if (error_or_void.is_error()) {
        warnln("Failed to mount {} (FD: {}) ({}) on {}: {}", filename, fd, fstype, mountpoint, error_or_void.error());
        return false;
    }

    return true;
}

static ErrorOr<void> mount_all()
{
    // Mount all filesystems listed in /etc/fstab.
    dbgln("Mounting all filesystems...");
    Array<u8, PAGE_SIZE> buffer;

    bool all_ok = true;
    auto process_fstab_entries = [&](StringView path) -> ErrorOr<void> {
        auto file_unbuffered = TRY(Core::Stream::File::open(path, Core::Stream::OpenMode::Read));
        auto file = TRY(Core::Stream::BufferedFile::create(move(file_unbuffered)));

        while (TRY(file->can_read_line())) {
            auto line = TRY(file->read_line(buffer));

            if (!mount_by_line(line))
                all_ok = false;
        }
        return {};
    };

    if (auto result = process_fstab_entries("/etc/fstab"sv); result.is_error())
        dbgln("Failed to read '/etc/fstab': {}", result.error());

    auto fstab_directory_iterator = Core::DirIterator("/etc/fstab.d", Core::DirIterator::SkipDots);

    if (fstab_directory_iterator.has_error() && fstab_directory_iterator.error() != ENOENT) {
        dbgln("Failed to open /etc/fstab.d: {}", fstab_directory_iterator.error_string());
    } else if (!fstab_directory_iterator.has_error()) {
        while (fstab_directory_iterator.has_next()) {
            auto path = fstab_directory_iterator.next_full_path();
            if (auto result = process_fstab_entries(path); result.is_error())
                dbgln("Failed to read '{}': {}", path, result.error());
        }
    }

    if (all_ok)
        return {};

    return Error::from_string_literal("One or more errors occurred. Please verify earlier output.");
}

static ErrorOr<void> print_mounts()
{
    // Output info about currently mounted filesystems.
    auto df = TRY(Core::Stream::File::open("/sys/kernel/df"sv, Core::Stream::OpenMode::Read));

    auto content = TRY(df->read_all());
    auto json = TRY(JsonValue::from_string(content));

    json.as_array().for_each([](auto& value) {
        auto& fs_object = value.as_object();
        auto class_name = fs_object.get("class_name"sv).to_string();
        auto mount_point = fs_object.get("mount_point"sv).to_string();
        auto source = fs_object.get("source"sv).as_string_or("none");
        auto readonly = fs_object.get("readonly"sv).to_bool();
        auto mount_flags = fs_object.get("mount_flags"sv).to_int();

        out("{} on {} type {} (", source, mount_point, class_name);

        if (readonly || mount_flags & MS_RDONLY)
            out("ro");
        else
            out("rw");

        if (mount_flags & MS_NODEV)
            out(",nodev");
        if (mount_flags & MS_NOREGULAR)
            out(",noregular");
        if (mount_flags & MS_NOEXEC)
            out(",noexec");
        if (mount_flags & MS_NOSUID)
            out(",nosuid");
        if (mount_flags & MS_BIND)
            out(",bind");
        if (mount_flags & MS_WXALLOWED)
            out(",wxallowed");
        if (mount_flags & MS_AXALLOWED)
            out(",axallowed");

        outln(")");
    });

    return {};
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView source;
    StringView mountpoint;
    StringView fs_type;
    StringView options;
    bool should_mount_all = false;

    Core::ArgsParser args_parser;
    args_parser.add_positional_argument(source, "Source path", "source", Core::ArgsParser::Required::No);
    args_parser.add_positional_argument(mountpoint, "Mount point", "mountpoint", Core::ArgsParser::Required::No);
    args_parser.add_option(fs_type, "File system type", nullptr, 't', "fstype");
    args_parser.add_option(options, "Mount options", nullptr, 'o', "options");
    args_parser.add_option(should_mount_all, "Mount all file systems listed in /etc/fstab and /etc/fstab.d/*", nullptr, 'a');
    args_parser.parse(arguments);

    if (should_mount_all) {
        TRY(mount_all());
        return 0;
    }

    if (source.is_empty() && mountpoint.is_empty()) {
        TRY(print_mounts());
        return 0;
    }

    if (!source.is_empty() && !mountpoint.is_empty()) {
        if (fs_type.is_empty())
            fs_type = "ext2"sv;
        int flags = !options.is_empty() ? parse_options(options) : 0;

        int const fd = TRY(get_source_fd(source));

        TRY(Core::System::mount(fd, mountpoint, fs_type, flags));

        return 0;
    }

    args_parser.print_usage(stderr, arguments.argv[0]);

    return 1;
}
