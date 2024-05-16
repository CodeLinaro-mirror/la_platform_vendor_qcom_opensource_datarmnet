load(":rmnetcore_modules.bzl", "rmnetcore_modules")
load(":module_mgr.bzl", "define_target_modules")

def define_mdm9607():
     define_target_modules(
        target = "mdm9607",
        variants = ["debug-defconfig", "perf-defconfig"],
        registry = rmnetcore_modules,
        modules = [
            "rmnet_core",
        ],
        config_options = []
    )
