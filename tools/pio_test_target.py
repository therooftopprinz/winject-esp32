# Host unit tests live in src/test/ (CMake + Google Test), not a PIO env.
Import("env")  # pylint: disable=undefined-variable

test_dir = "$PROJECT_DIR/src/test"
test_build = "$PROJECT_DIR/src/test/build"

env.AddCustomTarget(  # pylint: disable=undefined-variable
    name="test",
    dependencies=None,
    actions=[
        f"cmake -S {test_dir} -B {test_build}",
        f"cmake --build {test_build}",
        f"ctest --test-dir {test_build} --output-on-failure",
    ],
    title="test",
    description="Build and run host Google tests (src/test)",
    always_build=True,
)
