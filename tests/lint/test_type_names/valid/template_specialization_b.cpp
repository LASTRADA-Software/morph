struct SpecFixtureThingB {
    int x = 0;
};

template <>
struct morph::model::ModelTraits<SpecFixtureThingB> {
    static constexpr std::string_view typeId() { return "SpecFixtureThingB"; }
};
