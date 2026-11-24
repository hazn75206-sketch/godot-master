def can_build(env, platform):
    # Disable module for template builds (editor-only module)
    if env["target"] in ("template_release", "template_debug"):
        return False
    return True

def configure(env):
    pass