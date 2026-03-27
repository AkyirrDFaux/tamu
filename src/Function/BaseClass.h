void BaseClass::Destroy()
{
    switch (Type)
    {
    case ObjectTypes::Board:
        delete (BoardClass *)this;
        break;
    case ObjectTypes::Display:
        delete (DisplayClass *)this;
        break;
    case ObjectTypes::I2C:
        delete (DisplayClass *)this;
        break;
    case ObjectTypes::Input:
        delete (InputClass *)this;
        break;
    case ObjectTypes::Sensor:
        delete (SensorClass *)this;
        break;
    /*case ObjectTypes::LEDStrip:
        delete (LEDStripClass *)this;
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
        break;*/
    default:
        delete this;
        break;
    }
};

void BaseClass::Save() {
    /*
    ByteArray Data = ByteArray(*this).SubArray(1); // Remove Type of ID
    if (Data.Length % FLASH_PADDING != 0)
    {
        uint32_t NewLength = Data.Length + FLASH_PADDING - (Data.Length % FLASH_PADDING);
        char *NewArray = new char[NewLength];
        memcpy(NewArray, Data.Array, Data.Length);
        delete[] Data.Array;
        for (uint32_t Pad = Data.Length; Pad < NewLength; Pad++)
            NewArray[Pad] = 0;
        Data.Array = NewArray;
        Data.Length = NewLength;
    }
    HW::FlashSave(Data);
    */
};

BaseClass *CreateObject(Reference ID, ObjectTypes Type)
{
    switch (Type)
    {
    // Port is auto
    /*case ObjectTypes::Shape2D:
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
        return new Texture2D(ID, Flags);*/
    case ObjectTypes::Display:
        return new DisplayClass(ID);
    case ObjectTypes::I2C:
        return new I2CDeviceClass(ID);
    case ObjectTypes::Input:
        return new InputClass(ID);
    case ObjectTypes::Sensor:
        return new SensorClass(ID); /*
     case ObjectTypes::Program:
         return new Program(ID, Flags);*/
    default:
        ReportError(Status::InvalidType);
        return nullptr;
    }
}