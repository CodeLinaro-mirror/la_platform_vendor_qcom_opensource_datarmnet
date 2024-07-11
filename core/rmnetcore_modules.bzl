load(":module_mgr.bzl", "create_module_registry")
RMNETCORE_PATH = ""

rmnetcore_modules = create_module_registry([":rmnetcore_src_headers"])

rmnetcore_modules.register(
    name = "rmnet_core",
    path = RMNETCORE_PATH,
    srcs = ([
            "rmnet_config.c",
            "rmnet_handlers.c",
            "rmnet_descriptor.c",
            "rmnet_genl.c",
            "rmnet_map_command.c",
            "rmnet_map_data.c",
            "rmnet_module.c",
            "rmnet_vnd.c",
            "rmnet_ll.c",
            "rmnet_ll_ipa.c",
            "qmi_rmnet.c",
            "wda_qmi.c",
            "dfc_qmi.c",
            "dfc_qmap.c",
            "rmnet_qmap.c",
            "rmnet_ll_qmap.c",
            ]),
)

