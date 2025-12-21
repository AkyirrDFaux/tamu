Number LimitZeroToOne(Number Value)
{
    return min(max(Value, Number(0)), Number(1));
};

uint8_t LimitByte(int Number)
{
    return min(max(Number, 0), 255);
};

Number ByteToPercent(byte Value)
{
    return LimitZeroToOne(Number(Value) / 255);
};

uint8_t PercentToByte(Number Value)
{
    return LimitByte((int32_t)Value * 255);
};

uint8_t MultiplyBytePercentByte(byte Number, byte Percent)
{
    return (uint8_t)(((int)Number * (int)Percent) / 255);
};

String BoolToString(bool Value)
{
    return (Value ? "True" : "False");
}

bool BoolFromString(String Text)
{
    if(Text == "True")
        return true;
    return false;
}
