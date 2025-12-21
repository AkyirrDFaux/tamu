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
