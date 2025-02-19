## DMF_LegacyProtocol

-----------------------------------------------------------------------------------------------------------------------------------

#### Module Summary

A summary of the Module is documented here. More details are in the "[Module Remarks]" section below.

LegacyProtocol Module that makes it easy to copy entry point signatures and see the code that must be customized for every Module. This
document is an extension of that allows new documents to be made for new Modules.

-----------------------------------------------------------------------------------------------------------------------------------

#### Module Configuration

##### DMF_CONFIG_LegacyProtocol
````
typedef struct
{
  // TEMPLATE: Add context specifically used to open this DMF Module.
  //      (Remove LegacyProtocolDummy).
  //
  ULONG Dummy;
} DMF_CONFIG_LegacyProtocol;
````

##### Remarks

The Module's CONFIG structure is documented here.

-----------------------------------------------------------------------------------------------------------------------------------

#### Module Enumeration Types

##### Remarks

This section lists all the Enumeration Types specific to this Module that are accessible to Clients.

-----------------------------------------------------------------------------------------------------------------------------------

#### Module Callbacks

-----------------------------------------------------------------------------------------------------------------------------------

#### Module Methods

-----------------------------------------------------------------------------------------------------------------------------------

Each of the Module's Methods is located here.

-----------------------------------------------------------------------------------------------------------------------------------

#### Module IOCTLs

-----------------------------------------------------------------------------------------------------------------------------------

#### Module Remarks

* Detailed explanation about using the Module that Clients need to consider.

-----------------------------------------------------------------------------------------------------------------------------------

#### Module Implementation Details

-----------------------------------------------------------------------------------------------------------------------------------

#### Examples

* Examples of where the Module is used.
* ClientDriver
* Module

-----------------------------------------------------------------------------------------------------------------------------------

#### To Do

-----------------------------------------------------------------------------------------------------------------------------------

#### Module Category

LegacyProtocol

-----------------------------------------------------------------------------------------------------------------------------------

