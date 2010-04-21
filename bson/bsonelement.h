// BSONElement

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

namespace mongo {

/** BSONElement represents an "element" in a BSONObj.  So for the object { a : 3, b : "abc" },
    'a : 3' is the first element (key+value).
       
    The BSONElement object points into the BSONObj's data.  Thus the BSONObj must stay in scope
    for the life of the BSONElement.

    internals:
    <type><fieldName    ><value>
    -------- size() ------------
    -fieldNameSize-
    value()
    type()
*/
class BSONElement {
public:
    /** These functions, which start with a capital letter, throw a UserException if the 
        element is not of the required type. Example:

        string foo = obj["foo"].String(); // exception if not a string type or DNE
    */
    string String()  const { return chk(mongo::String).toString(); }
    Date_t Date()    const { return chk(mongo::Date).date(); }
    double Number()  const { return chk(isNumber()).number(); }
    double Double()  const { return chk(NumberDouble)._numberDouble(); }
    long long Long() const { return chk(NumberLong)._numberLong(); }
    int Int()        const { return chk(NumberInt)._numberInt(); }
    bool Bool()      const { return chk(mongo::Bool).boolean(); }
    BSONObj Obj()    const;
    mongo::OID OID() const { return chk(jstOID).__oid(); }
    void Null()      const { chk(isNull()); }

    string toString( bool includeFieldName = true ) const;
    operator string() const { return toString(); }
    string jsonString( JsonStringFormat format, bool includeFieldNames = true, int pretty = 0 ) const;

    /** Returns the type of the element */
    BSONType type() const { return (BSONType) *data; }
        
    /** returns the tyoe of the element fixed for the main type
        the main purpose is numbers.  any numeric type will return NumberDouble
        Note: if the order changes, indexes have to be re-built or than can be corruption
    */
    int canonicalType() const;

    /** Indicates if it is the end-of-object element, which is present at the end of 
        every BSON object. 
    */
    bool eoo() const {
        return type() == EOO;
    }

    /** Size of the element.
        @param maxLen If maxLen is specified, don't scan more than maxLen bytes to calculate size. 
    */
    int size( int maxLen = -1 ) const;

    /** Wrap this element up as a singleton object. */
    BSONObj wrap() const;

    /** Wrap this element up as a singleton object with a new name. */
    BSONObj wrap( const char* newName) const;

    /** field name of the element.  e.g., for 
        name : "Joe"
        "name" is the fieldname
    */
    const char * fieldName() const {
        if ( eoo() ) return ""; // no fieldname for it.
        return data + 1;
    }

    /** raw data of the element's value (so be careful). */
    const char * value() const {
        return (data + fieldNameSize() + 1);
    }
    /** size in bytes of the element's value (when applicable). */
    int valuesize() const {
        return size() - fieldNameSize() - 1;
    }

    bool isBoolean() const { return type() == mongo::Bool; }

    /** @return value of a boolean element.  
        You must assure element is a boolean before 
        calling. */
    bool boolean() const {
        return *value() ? true : false;
    }

    /** Retrieve a java style date value from the element. 
        Ensure element is of type Date before calling.
    */
    Date_t date() const {
        return *reinterpret_cast< const Date_t* >( value() );
    }

    /** Convert the value to boolean, regardless of its type, in a javascript-like fashion 
        (i.e., treat zero and null as false).
    */
    bool trueValue() const;

    /** True if number, string, bool, date, OID */
    bool isSimpleType() const;

    /** True if element is of a numeric type. */
    bool isNumber() const;

    /** Return double value for this field. MUST be NumberDouble type. */
    double _numberDouble() const {return *reinterpret_cast< const double* >( value() ); }
    /** Return double value for this field. MUST be NumberInt type. */
    int _numberInt() const {return *reinterpret_cast< const int* >( value() ); }
    /** Return double value for this field. MUST be NumberLong type. */
    long long _numberLong() const {return *reinterpret_cast< const long long* >( value() ); }

    /** Retrieve int value for the element safely.  Zero returned if not a number. */
    int numberInt() const;
    /** Retrieve long value for the element safely.  Zero returned if not a number. */
    long long numberLong() const;
    /** Retrieve the numeric value of the element.  If not of a numeric type, returns 0. 
        Note: casts to double, data loss may occur with large (>52 bit) NumberLong values.
    */
    double numberDouble() const;
    /** Retrieve the numeric value of the element.  If not of a numeric type, returns 0. 
        Note: casts to double, data loss may occur with large (>52 bit) NumberLong values.
    */
    double number() const { return numberDouble(); }

    /** Retrieve the object ID stored in the object. 
        You must ensure the element is of type jstOID first. */
    const mongo::OID &__oid() const { return *reinterpret_cast< const mongo::OID* >( value() ); }

    /** True if element is null. */
    bool isNull() const {
        return type() == jstNULL;
    }
        
    /** Size (length) of a string element.  
        You must assure of type String first.  */
    int valuestrsize() const {
        return *reinterpret_cast< const int* >( value() );
    }

    // for objects the size *includes* the size of the size field
    int objsize() const {
        return *reinterpret_cast< const int* >( value() );
    }

    /** Get a string's value.  Also gives you start of the real data for an embedded object. 
        You must assure data is of an appropriate type first -- see also valuestrsafe().
    */
    const char * valuestr() const {
        return value() + 4;
    }

    /** Get the string value of the element.  If not a string returns "". */
    const char *valuestrsafe() const {
        return type() == mongo::String ? valuestr() : "";
    }
    /** Get the string value of the element.  If not a string returns "". */
    string str() const { return valuestrsafe(); }

    /** Get javascript code of a CodeWScope data element. */
    const char * codeWScopeCode() const {
        return value() + 8;
    }
    /** Get the scope SavedContext of a CodeWScope data element. */
    const char * codeWScopeScopeData() const {
        // TODO fix
        return codeWScopeCode() + strlen( codeWScopeCode() ) + 1;
    }

    /** Get the embedded object this element holds. */
    BSONObj embeddedObject() const;

    /* uasserts if not an object */
    BSONObj embeddedObjectUserCheck() const;

    BSONObj codeWScopeObject() const;

    string ascode() const {
        switch( type() ){
        case mongo::String:
        case Code:
            return valuestr();
        case CodeWScope:
            return codeWScopeCode();
        default:
            log() << "can't convert type: " << (int)(type()) << " to code" << endl;
        }
        uassert( 10062 ,  "not code" , 0 );
        return "";
    }

    /** Get binary data.  Element must be of type BinData */
    const char *binData(int& len) const { 
        // BinData: <int len> <byte subtype> <byte[len] data>
        assert( type() == BinData );
        len = valuestrsize();
        return value() + 5;
    }
        
    BinDataType binDataType() const {
        // BinData: <int len> <byte subtype> <byte[len] data>
        assert( type() == BinData );
        unsigned char c = (value() + 4)[0];
        return (BinDataType)c;
    }

    /** Retrieve the regex string for a Regex element */
    const char *regex() const {
        assert(type() == RegEx);
        return value();
    }

    /** Retrieve the regex flags (options) for a Regex element */
    const char *regexFlags() const {
        const char *p = regex();
        return p + strlen(p) + 1;
    }

    /** like operator== but doesn't check the fieldname,
        just the value.
    */
    bool valuesEqual(const BSONElement& r) const {
        return woCompare( r , false ) == 0;
    }

    /** Returns true if elements are equal. */
    bool operator==(const BSONElement& r) const {
        return woCompare( r , true ) == 0;
    }


    /** Well ordered comparison.
        @return <0: l<r. 0:l==r. >0:l>r
        order by type, field name, and field value.
        If considerFieldName is true, pay attention to the field name.
    */
    int woCompare( const BSONElement &e, bool considerFieldName = true ) const;

    const char * rawdata() const {
        return data;
    }
        
    /** 0 == Equality, just not defined yet */
    int getGtLtOp( int def = 0 ) const;

    /** Constructs an empty element */
    BSONElement();
        
    /** Check that data is internally consistent. */
    void validate() const;

    /** True if this element may contain subobjects. */
    bool mayEncapsulate() const {
        switch ( type() ){
        case Object:
        case Array:
        case CodeWScope:
            return true;
        default:
            return false;
        }
    }

    /** True if this element can be a BSONObj */
    bool isABSONObj() const {
        switch( type() ){
        case Object:
        case Array:
            return true;
        default:
            return false;
        }
    }

    OpTime optime() const {
        return OpTime( *reinterpret_cast< const unsigned long long* >( value() ) );
    }

    Date_t timestampTime() const{
        unsigned long long t = ((unsigned int*)(value() + 4 ))[0];
        return t * 1000;
    }
    unsigned int timestampInc() const{
        return ((unsigned int*)(value() ))[0];
    }

    const char * dbrefNS() const {
        uassert( 10063 ,  "not a dbref" , type() == DBRef );
        return value() + 4;
    }

    const mongo::OID& dbrefOID() const {
        uassert( 10064 ,  "not a dbref" , type() == DBRef );
        const char * start = value();
        start += 4 + *reinterpret_cast< const int* >( start );
        return *reinterpret_cast< const mongo::OID* >( start );
    }

    bool operator<( const BSONElement& other ) const {
        int x = (int)canonicalType() - (int)other.canonicalType();
        if ( x < 0 ) return true;
        else if ( x > 0 ) return false;
        return compareElementValues(*this,other) < 0;
    }
        
    // If maxLen is specified, don't scan more than maxLen bytes.
    BSONElement(const char *d, int maxLen = -1) : data(d) {
        fieldNameSize_ = -1;
        if ( eoo() )
            fieldNameSize_ = 0;
        else {
            if ( maxLen != -1 ) {
                int size = strnlen( fieldName(), maxLen - 1 );
                massert( 10333 ,  "Invalid field name", size != -1 );
                fieldNameSize_ = size + 1;
            }
        }
        totalSize = -1;
    }
private:
    const char *data;
    mutable int fieldNameSize_; // cached value
    int fieldNameSize() const {
        if ( fieldNameSize_ == -1 )
            fieldNameSize_ = (int)strlen( fieldName() ) + 1;
        return fieldNameSize_;
    }
    mutable int totalSize; /* caches the computed size */

    friend class BSONObjIterator;
    friend class BSONObj;
    const BSONElement& chk(int t) const { 
        uassert(13111, "unexpected or missing type value in BSON object", t == type());
        return *this;
    }
    const BSONElement& chk(bool expr) const { 
        uassert(13118, "unexpected or missing type value in BSON object", expr);
        return *this;
    }
};


    inline int BSONElement::canonicalType() const {
        BSONType t = type();
        switch ( t ){
        case MinKey:
        case MaxKey:
            return t;
        case EOO:
        case Undefined:
            return 0;
        case jstNULL:
            return 5;
        case NumberDouble:
        case NumberInt:
        case NumberLong:
            return 10;
        case mongo::String:
        case Symbol:
            return 15;
        case Object:
            return 20;
        case Array:
            return 25;
        case BinData:
            return 30;
        case jstOID:
            return 35;
        case mongo::Bool:
            return 40;
        case mongo::Date:
        case Timestamp:
            return 45;
        case RegEx:
            return 50;
        case DBRef:
            return 55;
        case Code:
            return 60;
        case CodeWScope:
            return 65;
        default:
            assert(0);
            return -1;
        }
	}

    inline bool BSONElement::trueValue() const {
        switch( type() ) {
        case NumberLong:
            return *reinterpret_cast< const long long* >( value() ) != 0;
        case NumberDouble:
            return *reinterpret_cast< const double* >( value() ) != 0;
        case NumberInt:
            return *reinterpret_cast< const int* >( value() ) != 0;
        case mongo::Bool:
            return boolean();
        case EOO:
        case jstNULL:
        case Undefined:
            return false;
                
        default:
            ;
        }
        return true;
    }

    /** True if element is of a numeric type. */
    inline bool BSONElement::isNumber() const {
        switch( type() ) {
        case NumberLong:
        case NumberDouble:
        case NumberInt:
            return true;
        default: 
            return false;
        }
    }

    inline bool BSONElement::isSimpleType() const {
        switch( type() ){
        case NumberLong:
        case NumberDouble:
        case NumberInt:
        case mongo::String:
        case mongo::Bool:
        case mongo::Date:
        case jstOID:
            return true;
        default: 
            return false;
        }
    }

    inline double BSONElement::numberDouble() const {
        switch( type() ) {
        case NumberDouble:
            return _numberDouble();
        case NumberInt:
            return *reinterpret_cast< const int* >( value() );
        case NumberLong:
            return (double) *reinterpret_cast< const long long* >( value() );
        default:
            return 0;
        }
    }

    /** Retrieve int value for the element safely.  Zero returned if not a number. */
    inline int BSONElement::numberInt() const { 
        switch( type() ) {
        case NumberDouble:
            return (int) _numberDouble();
        case NumberInt:
            return _numberInt();
        case NumberLong:
            return (int) _numberLong();
        default:
            return 0;
        }
    }

    /** Retrieve long value for the element safely.  Zero returned if not a number. */
    inline long long BSONElement::numberLong() const { 
        switch( type() ) {
        case NumberDouble:
            return (long long) _numberDouble();
        case NumberInt:
            return _numberInt();
        case NumberLong:
            return _numberLong();
        default:
            return 0;
        }
    }    

}
