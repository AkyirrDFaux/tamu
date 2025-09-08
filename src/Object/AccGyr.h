//Temporary
class GyrAccClass : public Variable<GyrAccs>
{
public:
    Variable<uint8_t> CPin = Variable<uint8_t>(0, RandomID, Flags::Auto);
    Variable<uint8_t> DPin = Variable<uint8_t>(0, RandomID, Flags::Auto);

    Variable<float> GX = Variable<float>(0, RandomID, Flags::Auto);
    Variable<float> GY = Variable<float>(0, RandomID, Flags::Auto);
    Variable<float> GZ = Variable<float>(0, RandomID, Flags::Auto);

    Variable<float> AX = Variable<float>(0, RandomID, Flags::Auto);
    Variable<float> AY = Variable<float>(0, RandomID, Flags::Auto);
    Variable<float> AZ = Variable<float>(0, RandomID, Flags::Auto);

    GyrAccClass(uint8_t NewCPin, uint8_t NewDPin, GyrAccs NewDevType);
    bool Run();
};

GyrAccClass::GyrAccClass(uint8_t NewCPin, uint8_t NewDPin, GyrAccs NewDevType) : Variable(NewDevType) // Created by Board
{
    CPin = NewCPin;
    DPin = NewDPin;

    Type = Types::AccGyr;
    Name = "Acc&Gyr";
    Flags = Flags::Auto;

    AddModule(&CPin);
    CPin.Name = "Clock Pin";
    AddModule(&DPin);
    DPin.Name = "Data Pin";

    Wire.begin(DPin,CPin,10000); //da 4, cl 5
    if (!lsm6ds3trc.begin_I2C(0b1101011))// 0b1101011 or 0b1101010
        Serial.println("Failed to find LSM6DS3TR-C chip");

    lsm6ds3trc.setAccelDataRate(LSM6DS_RATE_104_HZ);
    lsm6ds3trc.setGyroDataRate(LSM6DS_RATE_104_HZ);
    lsm6ds3trc.setAccelRange(LSM6DS_ACCEL_RANGE_16_G);
    lsm6ds3trc.setGyroRange(LSM6DS_GYRO_RANGE_2000_DPS);

    AddModule(&AX);
    AX.Name = "Acc X";
    AddModule(&AY);
    AY.Name = "Acc Y";
    AddModule(&AZ);
    AZ.Name = "Acc Z";

    AddModule(&GX);
    GX.Name = "Gyr X";
    AddModule(&GY);
    GY.Name = "Gyr Y";
    AddModule(&GZ);
    GZ.Name = "Gyr Z";

    Sensors.Add(this);
};

bool GyrAccClass::Run(){
    sensors_event_t accel;
    sensors_event_t gyro;
    sensors_event_t temp;
    if(lsm6ds3trc.getEvent(&accel, &gyro, &temp))
    {
        //Compensate for gravity
        AX = accel.acceleration.x;
        AY = accel.acceleration.y;
        AZ = accel.acceleration.z;

        GX = gyro.gyro.x;
        GY = gyro.gyro.y;
        GZ = gyro.gyro.z;
        
        //Add orientation from gravity
    }
    return true;
}