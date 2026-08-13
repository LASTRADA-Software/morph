// A template specialization's name always carries its own qualifying
// namespace (morph::model::ModelTraits<...>) and specializes an existing
// template rather than declaring a new type, so it is excluded from the
// file-scope collision scan -- confirmed here by declaring the *specialized*
// type (RealThing) with a name unique to this fixture, so only the
// specialization line itself exercises the exclusion.
struct SpecFixtureThingA {
    int x = 0;
};

template <>
struct morph::model::ModelTraits<SpecFixtureThingA> {
    static constexpr std::string_view typeId() { return "SpecFixtureThingA"; }
};
