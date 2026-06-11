def pytest_addoption(parser):
    parser.addoption('--enable-slow-tests',
                     action='store_true',
                     dest="enable-slow-tests",
                     default=False,
                     help="enable long-running tests")


def pytest_generate_tests(metafunc):
    # The reflection suite's `t` fixture: the bound test module, parameterized
    # over both binder backends -- the constexpr binder (test_reflect_ext) and
    # the emit backend's generated-source module (test_reflect_emit_ext).
    # importorskip keeps non-reflection builds green.
    if "t" in metafunc.fixturenames:
        metafunc.parametrize("t_backend", ["constexpr", "emit"], indirect=False)


import pytest


@pytest.fixture
def t(t_backend):
    name = ("test_reflect_ext" if t_backend == "constexpr"
            else "test_reflect_emit_ext")
    return pytest.importorskip(name)
