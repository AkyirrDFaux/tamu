void BaseClass::Destroy()
{
    switch (Type)
    {
    case ObjectTypes::Shape2D:
        delete (Shape2DClass *)this;
        break;
    case ObjectTypes::Board:
        delete (BoardClass *)this;
        break;
    case ObjectTypes::Port:
        delete (PortClass *)this;
        break;
    case ObjectTypes::Fan:
        delete (FanClass *)this;
        break;
    case ObjectTypes::LEDStrip:
        delete (LEDStripClass *)this;
        break;
    case ObjectTypes::LEDSegment:
        delete (LEDSegmentClass *)this;
        break;
    case ObjectTypes::Texture1D:
        delete (Texture1D *)this;
        break;
    case ObjectTypes::Display:
        delete (DisplayClass *)this;
        break;
    case ObjectTypes::Geometry2D:
        delete (Geometry2DClass *)this;
        break;
    case ObjectTypes::Texture2D:
        delete (Texture2D *)this;
        break;
    case ObjectTypes::AccGyr:
        delete (GyrAccClass *)this;
        break;
    case ObjectTypes::Servo:
        delete (ServoClass *)this;
        break;
    case ObjectTypes::Input:
        delete (InputClass *)this;
        break;
    case ObjectTypes::Operation:
        delete (Operation *)this;
        break;
    case ObjectTypes::Program:
        delete (Program *)this;
        break;
    case ObjectTypes::I2C:
        delete (I2CClass *)this;
        break;
    case ObjectTypes::UART:
        delete (UARTClass *)this;
        break;
    case ObjectTypes::SPI:
        // delete (SPIClass *)this;
        break;
    case ObjectTypes::Sensor:
        delete (SensorClass *)this;
        break;
    default:
        delete this;
        break;
    }
};

BaseClass *CreateObject(ObjectTypes Type, bool New, IDClass ID, FlagClass Flags)
{
    switch (Type)
    {
    // Port is auto
    case ObjectTypes::Shape2D:
        return new Shape2DClass(ID, Flags);
    // case ObjectTypes::Board: //Cannot create board
    case ObjectTypes::Fan:
        return new FanClass(ID, Flags);
    case ObjectTypes::LEDSegment:
        return new LEDSegmentClass(ID, Flags);
    case ObjectTypes::Texture1D:
        return new Texture1D(ID, Flags);
    case ObjectTypes::LEDStrip:
        return new LEDStripClass(ID, Flags);
    case ObjectTypes::Geometry2D:
        return new Geometry2DClass(ID, Flags);
    case ObjectTypes::Texture2D:
        return new Texture2D(ID, Flags);
    case ObjectTypes::Display:
        return new DisplayClass(ID, Flags);
    case ObjectTypes::AccGyr:
        return new GyrAccClass(ID, Flags);
    case ObjectTypes::Input:
        return new InputClass(ID, Flags);
    case ObjectTypes::Operation:
        return new Operation(ID, Flags);
    case ObjectTypes::Program:
        return new Program(ID, Flags);
    default:
        ReportError(Status::InvalidType);
        return nullptr;
    }
}