template <class C>
class Variable : public BaseClass
{
public:
    C *Data;
    Variable(C New, IDClass ID = RandomID, FlagClass Flags = Flags::None) : BaseClass(ID, Flags)
    {
        Type = GetType<C>();
        Data = new C;
        *Data = New;
    };
    ~Variable()
    {
        delete Data;
    }

    operator C() { return *Data; }; // OMG :D, but unsafe!
    C *Get() { return Data; }
    void operator=(C Other) { *Data = Other; };
    bool operator==(C Other) { return Other == *Data; };
};