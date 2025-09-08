//Temporary
class GyrAccClass : public Variable<GyrAccs>
{
public:
    Variable<uint8_t> CPin = Variable<uint8_t>(0, RandomID, Flags::Auto);
    Variable<uint8_t> DPin = Variable<uint8_t>(0, RandomID, Flags::Auto);

    Variable<Vector3D> AR = Variable<Vector3D>(Vector3D(0,0,0), RandomID, Flags::Auto);
    Variable<Vector3D> AC = Variable<Vector3D>(Vector3D(0,0,0), RandomID, Flags::Auto);

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

    AddModule(&AC);
    AC.Name = "Acceleration";
    AddModule(&AR);
    AR.Name = "Angular rate";

    Sensors.Add(this);
};

bool GyrAccClass::Run(){
    sensors_event_t a;
    sensors_event_t g;
    sensors_event_t t;
    if(lsm6ds3trc.getEvent(&a, &g, &t))
    {
        //Compensate for gravity
        AC = Vector3D(a.acceleration.x,a.acceleration.y,a.acceleration.z);
        AR = Vector3D(g.gyro.x,g.gyro.y,g.gyro.z);
        
        //Add orientation from gravity
    }
    return true;
}