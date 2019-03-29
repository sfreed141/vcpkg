#include "pch.h"

#include <vcpkg/base/checks.h>
#include <vcpkg/base/files.h>
#include <vcpkg/base/optional.h>
#include <vcpkg/base/stringliteral.h>
#include <vcpkg/base/system.h>

#include <vcpkg/archives.h>
#include <vcpkg/commands.h>
#include <vcpkg/help.h>
#include <vcpkg/paragraphs.h>
#include <vcpkg/sourceparagraph.h>

namespace vcpkg::Commands::AnalyzePackage
{
    static constexpr StringLiteral OPTION_QUIET = "--quiet";

    static constexpr StringLiteral OPTION_OUTFILE = "--outfile";

    static constexpr std::array<CommandSwitch, 1> ANALYZE_SWITCHES = {{
        {OPTION_QUIET, "Suppresses extra status messages"},
    }};

    static constexpr std::array<CommandSetting, 1> ANALYZE_SETTINGS = {{
        {OPTION_OUTFILE, "Output to file instead of stdout"}
    }};

    const CommandStructure COMMAND_STRUCTURE = {
        Strings::format(
            "Analyzes and outputs CMake usage information from one or more provided zipped packages.\n%s",
            Help::create_example_string("x-analyze-package package.zip")),
        1,
        SIZE_MAX,
        { ANALYZE_SWITCHES, ANALYZE_SETTINGS },
        nullptr
    };

    // Map of CMake find_package names to related config files
    using ConfigMap = std::map<std::string, std::string>;

    // Map of CMake find_package names to the package's provided targets
    using TargetMap = std::map<std::string, std::vector<std::string>>;

    // Flag to suppress status messages
    static bool quiet_output;

    struct CMakeInfo
    {
        std::string port_name, port_description, usage;
        ConfigMap config_files;
        TargetMap library_targets;
    };

    static void parse_cmake_targets(const std::vector<fs::path>& files, const VcpkgPaths& paths, ConfigMap& config_files, TargetMap& library_targets)
    {
        static const std::regex cmake_library_regex(R"(\badd_library\(([^\s\)]+)\s)", std::regex_constants::ECMAScript);

        const auto& fs = paths.get_filesystem();

        for (auto& path : files)
        {
            // Only search for CMake targets in .cmake files
            if (Strings::case_insensitive_ascii_contains(path.generic_string(), "/share/") && Strings::ends_with(path.generic_string(), ".cmake"))
            {
                auto maybe_contents = fs.read_contents(path);
                auto find_package_name = path.parent_path().filename().u8string();

                // Find all library targets
                if (auto p_contents = maybe_contents.get())
                {
                    std::sregex_iterator next(p_contents->begin(), p_contents->end(), cmake_library_regex);
                    std::sregex_iterator last;

                    while (next != last)
                    {
                        auto match = *next;
                        library_targets[find_package_name].push_back(match[1]);
                        ++next;
                    }
                }

                auto filename = fs::u8path(path.generic_string()).filename().u8string();

                if (Strings::ends_with(filename, "Config.cmake"))
                {
                    auto root = filename.substr(0, filename.size() - 12);
                    if (Strings::case_insensitive_ascii_equals(root, find_package_name))
                        config_files[find_package_name] = root;
                }
                else if (Strings::ends_with(filename, "-config.cmake"))
                {
                    auto root = filename.substr(0, filename.size() - 13);
                    if (Strings::case_insensitive_ascii_equals(root, find_package_name))
                        config_files[find_package_name] = root;
                }
            }
        }
    }

    static CMakeInfo get_cmake_information(const fs::path& package_root, const VcpkgPaths& paths) {
        const auto& fs = paths.get_filesystem();

        CMakeInfo cmake_info;

        // Parse the CONTROL file to get basic metadata
        const auto control_path = package_root / "CONTROL";
        auto pghs = Paragraphs::get_paragraphs(fs, control_path);
        if (auto p = pghs.get())
        {
            auto first = (*p)[0];

            // Source CONTROL file
            if (first.count("Source") > 0)
            {
                cmake_info.port_name = first["Source"];
            }
            // Binary CONTROL file
            else if (first.count("Package") > 0)
            {
                cmake_info.port_name = first["Package"];
            }

            if (first.count("Description") > 0)
            {
                cmake_info.port_description = first["Description"];
            }
        }
        else
        {
            Checks::exit_with_message(VCPKG_LINE_INFO,
                Strings::format("Error parsing CONTROL file '%s': %s", control_path.generic_string(), pghs.error().message()));
        }

        // Check and use the usage file, if one exists
        const auto files = fs.get_files_recursive(package_root / "share");
        const auto usage_iter = std::find_if(files.cbegin(), files.cend(), [](const auto& f) { return f.filename().string() == "usage"; });
        if (usage_iter != files.cend())
        {
            if (const auto usage_file = *usage_iter; fs.exists(usage_file))
            {
                auto maybe_contents = fs.read_contents(usage_file);
                if (auto p_contents = maybe_contents.get())
                {
                    cmake_info.usage = *p_contents;

                    // escape all newlines
                    // TODO gotta be a better way? also small optimization use pos in find arg to avoid researching beginning of string (dont forget reset for loop 2)
                    size_t pos = 0;
                    while ((pos = cmake_info.usage.find("\r\n", pos)) != std::string::npos) {
                        cmake_info.usage.replace(pos, 2, "\\r\\n", 4);
                    }

                    pos = 0;
                    while ((pos = cmake_info.usage.find("\n"), pos) != std::string::npos) {
                        cmake_info.usage.replace(pos, 1, "\\n", 2);
                    }
                }
            }
        }

        // Search for CMake targets in the .cmake files
        parse_cmake_targets(files, paths, cmake_info.config_files, cmake_info.library_targets);

        if (cmake_info.library_targets.empty())
        {
            // If the port only provides a usage file, we try to find any packages it might provide
            static const std::regex package_name_regex(R"(\bfind_package\(([^\s\)]+)\s)", std::regex_constants::ECMAScript);

            std::sregex_iterator next(cmake_info.usage.cbegin(), cmake_info.usage.cend(), package_name_regex);
            std::sregex_iterator last;

            while (next != last)
            {
                auto match = *next;
                cmake_info.library_targets.emplace(match[1], std::vector<std::string>{});

                // TODO Scan for targets provided by the package
                // std::regex target_regex(R"()", std::regex_constants::ECMAScript);
                // std::sregex_iterator next_target(cmake_info.usage.cbegin(), cmake_info.usage.cend(), target_regex);
                // std::sregex_iterator last_target;

                // std::vector<std::string> targets;
                // while (next_target != last_target)
                // {
                //     auto target_match = *next_target;
                //     targets.push_back(target_match[2]);
                //     ++next_target;
                // }
                // cmake_info.library_targets.emplace(match[1], targets);

                ++next;
            }
        }

        return cmake_info;
    }

    static std::tuple<std::string, std::vector<std::string>> generate_cmake_info_json(CMakeInfo& cmake_info)
    {
        std::vector<std::string> package_strs;
        if (cmake_info.library_targets.empty())
        {
            // If no packages found, they probably have to find and link manually. Enter a dummy value in the json just so we know about the port
            package_strs.push_back(Strings::format(
                R"(    "_%s": { "name": "_%s", "targets": [], "portName": "%s", "description": "%s" })",
                cmake_info.port_name, cmake_info.port_name, cmake_info.port_name, cmake_info.usage));
        }
        else
        {
            package_strs.reserve(cmake_info.library_targets.size());

            for (auto&& library_target_pair : cmake_info.library_targets)
            {
                auto config_it = cmake_info.config_files.find(library_target_pair.first);

                const auto package_name = config_it != cmake_info.config_files.end() ? config_it->second : library_target_pair.first;

                // sort the target names alphabetically (to make output deterministic)
                std::sort(library_target_pair.second.begin(), library_target_pair.second.end());

                // If no usage message then generate one from the known package and targets
                if (cmake_info.usage.empty())
                {
                    cmake_info.usage = Strings::format("The package %s provides CMake targets:\\r\\n\\r\\n    find_package(%s CONFIG REQUIRED)\\r\\n    target_link_libraries(main PRIVATE %s)\\r\\n",
                        cmake_info.port_name, package_name, Strings::join(" ", library_target_pair.second));
                }

                package_strs.push_back(Strings::format(
                    R"(    "%s": { "name": "%s", "targets": ["%s"], "portName": "%s", "description": "%s" })",
                    package_name, package_name, Strings::join("\", \"", library_target_pair.second), cmake_info.port_name, cmake_info.usage));
            }
        }

        return std::make_pair(cmake_info.port_name, package_strs);
    }

    void perform_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths)
    {
        const ParsedArguments options = args.parse_arguments(COMMAND_STRUCTURE);

        // Check if we should suppress status messages
        quiet_output = Util::Sets::contains(options.switches, OPTION_QUIET);

        // Check if we should write to file instead of stdout
        std::ofstream outfile;
        auto it_outfile = options.settings.find(OPTION_OUTFILE);
        if (it_outfile != options.settings.end())
        {
            auto outfile_path = std::filesystem::canonical(fs::path(it_outfile->second));

            try
            {
                outfile.open(outfile_path);
            }
            catch (std::ios_base::failure e)
            {
                Checks::check_exit(VCPKG_LINE_INFO, outfile.is_open(),
                    Strings::format("Failed opening output file '%s': %s", outfile_path.string(), e.what()));
            }

            if (!quiet_output)
                System::println("Output will be written to '%s'.", outfile_path.string());
        }

        // The zips will be extracted to a temporary directory (and deleted later)
        std::error_code error;
        const auto temp_dir = std::filesystem::temp_directory_path(error) / "vcpkg";
        Checks::check_exit(VCPKG_LINE_INFO, !error,
            "Failed opening temp directory: %s", error.message());

        std::vector<std::string> packages;
        for (const auto& path_str : args.command_arguments)
        {
            if (!quiet_output)
                System::print(Strings::format("Processing %s...", path_str));

            const auto path = fs::path(path_str);
            const auto to_path = temp_dir / path.stem();

            // TODO only extract necessary files
            Archives::extract_archive(paths, path, to_path);

            auto cmake_info = get_cmake_information(to_path, paths);
            auto [portName, package_strs] = generate_cmake_info_json(cmake_info);

            if (!quiet_output)
            {
                System::println("done (port '%s' provides %d target%s)",
                    portName, package_strs.size(), package_strs.size() == 1 ? "" : "s");
            }

            packages.insert(packages.cend(), package_strs.begin(), package_strs.end());
        }

        auto output = Strings::format("%s,\n", Strings::join(",\n", packages));
        // auto output = Strings::format("{\n%s\n}\n", Strings::join(",\n", packages));

        if (outfile.is_open())
        {
            outfile << output;
            outfile.close();
        }
        else
        {
            System::println(output);
        }

        std::filesystem::remove_all(temp_dir, error);
        Checks::check_exit(VCPKG_LINE_INFO, !error,
            "Failed removing temp directory: %s", error.message());

        Checks::exit_success(VCPKG_LINE_INFO);
    }
}
