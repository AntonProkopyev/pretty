probe_resource = program(
    srcs=["$(S)/probe.cpp", "$(S)/probe.rc"],
)

group("probe", probe_resource)
install(probe_resource)
