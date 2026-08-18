#pragma once

#include <stdexcept>
#include <string>

namespace pgquarry
{

enum class JobType { Embed, Generate };

inline const char* to_string(JobType t)
{
    return t == JobType::Generate ? "generate" : "embed";
}

inline JobType job_type_from_string(const std::string& s)
{
    if (s == "embed")    return JobType::Embed;
    if (s == "generate") return JobType::Generate;
    throw std::runtime_error("job_type_from_string: unknown job_type '" + s + "'");
}

} // namespace pgquarry
