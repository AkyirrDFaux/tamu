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

Number TimeStep(uint32_t TargetTime)
{
    if (CurrentTime >= TargetTime)
        return 1;

    uint32_t RemainingTime = TargetTime - CurrentTime;

    if (RemainingTime == 0)
        return 0;

    Number Prediction = Number(DeltaTime) / Number(RemainingTime);

    return LimitZeroToOne(Prediction);
};

Number TimeMove(Number Current, Number Target, uint32_t TargetTime)
{
    return Current + (Target - Current) * TimeStep(TargetTime);
};

Number LimitPi(Number Value){
    while (Value > PI)
        Value -= 2*PI;
    while (Value < -PI)
        Value += 2*PI;
    return Value;
}
