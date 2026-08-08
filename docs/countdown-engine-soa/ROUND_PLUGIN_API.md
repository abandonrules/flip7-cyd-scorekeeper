# Round Plug-in API

## Required interface

Every round plug-in must provide:

- unique type ID
- version
- initial-state factory
- command handler
- event reducer
- snapshot serializer
- result producer
- completion flag

```cpp
struct RoundDescriptor {
    std::string typeId;
    std::string version;
    std::string minimumEngineVersion;
};

class IRoundPlugin {
public:
    virtual ~IRoundPlugin() = default;

    virtual RoundDescriptor descriptor() const = 0;
    virtual RoundState create(
        const RoundConfig& config,
        IRandomSource& random
    ) const = 0;

    virtual CommandResult handle(
        const RoundState& state,
        const RoundCommand& command,
        const CommandContext& context
    ) const = 0;

    virtual RoundState reduce(
        const RoundState& state,
        const RoundEvent& event
    ) const = 0;

    virtual RoundResult result(
        const RoundState& state
    ) const = 0;
};
```

## Plug-in rules

A plug-in must not:

- draw UI
- access hardware directly
- call the system clock directly
- access networking directly
- mutate match totals directly
- bypass command validation
