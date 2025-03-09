float LimitZeroToOne(float Number)
{
    return min(max(Number, (float)0), (float)1);
};

uint8_t LimitByte(int Number)
{
    return min(max(Number, 0), 255);
};

float ByteToPercent(byte Number)
{
    return LimitZeroToOne((float)Number / 255);
};

uint8_t PercentToByte(float Number)
{
    return LimitByte((int)Number * 255);
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
