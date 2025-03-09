float TimeStep(uint32_t TargetTime)
{
    if (CurrentTime >= TargetTime)
        return 1;

    uint32_t RemainingTime = TargetTime - CurrentTime;

    float Prediction = (float)DeltaTime / (float)RemainingTime;

    if (Prediction == NAN || Prediction == INFINITY)
        return 0;

    return LimitZeroToOne(Prediction);
};

float TimeMove(float Current, float Target, uint32_t TargetTime)
{
    return Current + (Target - Current) * TimeStep(TargetTime);
};

float LimitPi(float Value){
    while (Value > PI)
        Value -= 2*PI;
    while (Value < -PI)
        Value += 2*PI;
    return Value;
}
