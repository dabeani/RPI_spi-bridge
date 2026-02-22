Import("env")
import os
from SCons.Script import AlwaysBuild

PROJECT_DIR = env.subst("$PROJECT_DIR")

def _run(cmd):
    print(">>", cmd)
    ret = os.system(cmd)
    if ret != 0:
        raise SystemExit(ret)

def build_module(source, target, env_):
    _run("make -C {}/src".format(PROJECT_DIR))

def clean_module(source, target, env_):
    _run("make -C {}/src clean".format(PROJECT_DIR))

def build_deb(source, target, env_):
    _run("{}/build-deb.sh".format(PROJECT_DIR))

env.AddCustomTarget(
    name="kmod",
    dependencies=None,
    actions=[build_module],
    title="Build kernel module (spibridge.ko)",
    description="Invokes kernel build system via make -C src"
)

env.AddCustomTarget(
    name="kmodclean",
    dependencies=None,
    actions=[clean_module],
    title="Clean kernel module build artifacts",
    description="Runs make clean in src"
)

env.AddCustomTarget(
    name="deb",
    dependencies=None,
    actions=[build_deb],
    title="Build Debian package (.deb)",
    description="Runs dpkg-buildpackage via build-deb.sh"
)

AlwaysBuild(env.Alias("kmod", None, build_module))
AlwaysBuild(env.Alias("kmodclean", None, clean_module))
AlwaysBuild(env.Alias("deb", None, build_deb))
