extern "C" int runtime_fixture_value();

extern "C" int runtime_component_value() { return runtime_fixture_value(); }
