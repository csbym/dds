//engine_v8.cpp

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

#include "engine_v8.h"

#include "v8_wrapper.h"
#include "v8_utils.h"
#include "v8_db.h"

#define V8_SIMPLE_HEADER V8Lock l; HandleScope handle_scope; Context::Scope context_scope( _context );

namespace mongo {

    // guarded by v8 mutex
    map< unsigned, int > __interruptSpecToThreadId;

    /**
     * Unwraps a BSONObj from the JS wrapper
     */
    static BSONObj* unwrapBSONObj(const Handle<v8::Object>& obj) {
      Handle<External> field = Handle<External>::Cast(obj->GetInternalField(0));
      if (field.IsEmpty() || !field->IsExternal())
          return 0;
      void* ptr = field->Value();
      return static_cast<BSONObj*>(ptr);
    }

    static Handle<v8::Value> namedGet(Local<v8::String> name, const v8::AccessorInfo &info) {
      if (info.This()->HasRealNamedProperty(name)) {
        return info.This()->GetRealNamedProperty(name);
      }

      string key = toSTLString(name);
      BSONObj *obj = unwrapBSONObj(info.Holder());
      if (!obj || !obj->hasElement(key.c_str()))
          return Handle<Value>();
      BSONElement elmt = obj->getField(key.c_str());
      Local< External > scp = External::Cast( *info.Data() );
      V8Scope* scope = (V8Scope*)(scp->Value());
      Handle<Value> val = scope->mongoToV8Element(elmt, true);
      info.This()->ForceSet(name, val, DontEnum);
      return val;
    }

//    static Handle<v8::Value> namedSet(Local<v8::String> name, Local<v8::Value> value_obj, const v8::AccessorInfo& info) {
//      return Handle<Value>();
//    }

    static Handle<v8::Array> namedEnumerator(const AccessorInfo &info) {
        BSONObj *obj = unwrapBSONObj(info.Holder());
        Handle<v8::Array> arr = Handle<v8::Array>(v8::Array::New(obj->nFields()));
        int i = 0;
        Local< External > scp = External::Cast( *info.Data() );
        V8Scope* scope = (V8Scope*)(scp->Value());
        // note here that if keys are parseable number, v8 will access them using index
        for ( BSONObjIterator it(*obj); it.more(); ++i) {
            const BSONElement& f = it.next();
//            arr->Set(i, v8::String::NewExternal(new ExternalString(f.fieldName())));
            Handle<v8::String> name = scope->getV8Str(f.fieldName());
            arr->Set(i, name);
        }
        return arr;
    }

//    v8::Handle<v8::Integer> namedQuery(Local<v8::String> property, const AccessorInfo& info) {
//      string key = ToString(property);
//      return v8::Integer::New(None);
//    }

    static Handle<v8::Value> indexedGet(uint32_t index, const v8::AccessorInfo &info) {
        StringBuilder ss;
        ss << index;
        string key = ss.str();
        Local< External > scp = External::Cast( *info.Data() );
        V8Scope* scope = (V8Scope*)(scp->Value());
        Handle<v8::String> name = scope->getV8Str(key);
        // v8 API really confusing here, must check existence on index, but then fetch with name
        if (info.This()->HasRealIndexedProperty(index))
            return info.This()->GetRealNamedProperty(name);
        BSONObj *obj = unwrapBSONObj(info.Holder());
        if (!obj || !obj->hasElement(key.c_str()))
          return Handle<Value>();
        BSONElement elmt = obj->getField(key);
        Handle<Value> val = scope->mongoToV8Element(elmt, true);
        info.This()->ForceSet(name, val);
        return val;
    }

//    static Handle<v8::Value> indexedSet(uint32_t index, Local<v8::Value> value_obj, const v8::AccessorInfo& info) {
//      return Handle<Value>();
//    }

    static Handle<v8::Array> indexedEnumerator(const AccessorInfo &info) {
        BSONObj *obj = unwrapBSONObj(info.Holder());
        Handle<v8::Array> arr = Handle<v8::Array>(v8::Array::New(obj->nFields()));
        Local< External > scp = External::Cast( *info.Data() );
        V8Scope* scope = (V8Scope*)(scp->Value());
        int i = 0;
        for ( BSONObjIterator it(*obj); it.more(); ++i) {
            const BSONElement& f = it.next();
//          arr->Set(i, v8::String::NewExternal(new ExternalString(f.fieldName())));
            arr->Set(i, scope->getV8Str(f.fieldName()));
        }
        return arr;
    }

    // --- engine ---

    V8ScriptEngine::V8ScriptEngine() {
    }

    V8ScriptEngine::~V8ScriptEngine() {
    }

    void ScriptEngine::setup() {
        if ( !globalScriptEngine ) {
            globalScriptEngine = new V8ScriptEngine();
        }
    }

    void V8ScriptEngine::interrupt( unsigned opSpec ) {
        v8::Locker l;
        if ( __interruptSpecToThreadId.count( opSpec ) ) {
            V8::TerminateExecution( __interruptSpecToThreadId[ opSpec ] );
        }
    }
    void V8ScriptEngine::interruptAll() {
        v8::Locker l;
        vector< int > toKill; // v8 mutex could potentially be yielded during the termination call
        for( map< unsigned, int >::const_iterator i = __interruptSpecToThreadId.begin(); i != __interruptSpecToThreadId.end(); ++i ) {
            toKill.push_back( i->second );
        }
        for( vector< int >::const_iterator i = toKill.begin(); i != toKill.end(); ++i ) {
            V8::TerminateExecution( *i );
        }
    }

    // --- scope ---

    V8Scope::V8Scope( V8ScriptEngine * engine )
        : _engine( engine ) ,
          _connectState( NOT ) {

        V8Lock l;
        HandleScope handleScope;
        _context = Context::New();
        Context::Scope context_scope( _context );
        _global = Persistent< v8::Object >::New( _context->Global() );
        _this = Persistent< v8::Object >::New( v8::Object::New() );

        // initialize lazy object template
        lzObjectTemplate = Persistent<ObjectTemplate>::New(ObjectTemplate::New());
        lzObjectTemplate->SetInternalFieldCount( 1 );
        lzObjectTemplate->SetNamedPropertyHandler(namedGet, 0, 0, 0, namedEnumerator, v8::External::New(this));
        lzObjectTemplate->SetIndexedPropertyHandler(indexedGet, 0, 0, 0, 0, v8::External::New(this));

        // initialize lazy array template
        // unfortunately it is not possible to create true v8 array from a template
        // this means we use an object template and copy methods over
        // this it creates issues when calling certain methods that check array type
        lzArrayTemplate = Persistent<ObjectTemplate>::New(ObjectTemplate::New());
        lzArrayTemplate->SetInternalFieldCount( 1 );
        lzArrayTemplate->SetIndexedPropertyHandler(indexedGet, 0, 0, 0, indexedEnumerator, v8::External::New(this));

        V8STR_CONN = getV8Str( "_conn" );
        V8STR_ID = getV8Str( "_id" );
        V8STR_LENGTH = getV8Str( "length" );
        V8STR_ISOBJECTID = getV8Str( "isObjectId" );
        V8STR_RETURN = getV8Str( "return" );
        V8STR_ARGS = getV8Str( "args" );
        V8STR_T = getV8Str( "t" );
        V8STR_I = getV8Str( "i" );
        V8STR_EMPTY = getV8Str( "" );
        V8STR_MINKEY = getV8Str( "$MinKey" );
        V8STR_MAXKEY = getV8Str( "$MaxKey" );
        V8STR_NUMBERLONG = getV8Str( "__NumberLong" );
        V8STR_DBPTR = getV8Str( "__DBPointer" );
        V8STR_BINDATA = getV8Str( "__BinData" );
        V8STR_NATIVE_FUNC = getV8Str( "_native_function" );
        V8STR_V8_FUNC = getV8Str( "_v8_function" );

        injectV8Function("print", Print);
        injectV8Function("version", Version);
        injectV8Function("load", load);

        _wrapper = Persistent< v8::Function >::New( getObjectWrapperTemplate(this)->GetFunction() );

        injectV8Function("gc", GCV8);

        installDBTypes( this, _global );
    }

    V8Scope::~V8Scope() {
        V8Lock l;
        Context::Scope context_scope( _context );
        _wrapper.Dispose();
        _this.Dispose();
        for( unsigned i = 0; i < _funcs.size(); ++i )
            _funcs[ i ].Dispose();
        _funcs.clear();
        _global.Dispose();
        _context.Dispose();
        std::map <string, v8::Persistent <v8::String> >::iterator it = _strCache.begin();
        std::map <string, v8::Persistent <v8::String> >::iterator end = _strCache.end();
        while (it != end) {
            it->second.Dispose();
            ++it;
        }
    }

    /**
     * JS Callback that will call a c++ function with BSON arguments.
     */
    Handle< Value > V8Scope::nativeCallback( V8Scope* scope, const Arguments &args ) {
        V8Lock l;
        HandleScope handle_scope;
        Local< External > f = External::Cast( *args.Callee()->Get( scope->V8STR_NATIVE_FUNC ) );
        NativeFunction function = (NativeFunction)(f->Value());
        BSONObjBuilder b;
        for( int i = 0; i < args.Length(); ++i ) {
            stringstream ss;
            ss << i;
            scope->v8ToMongoElement( b, scope->V8STR_EMPTY, ss.str(), args[ i ] );
        }
        BSONObj nativeArgs = b.obj();
        BSONObj ret;
        try {
            ret = function( nativeArgs );
        }
        catch( const std::exception &e ) {
            return v8::ThrowException(v8::String::New(e.what()));
        }
        catch( ... ) {
            return v8::ThrowException(v8::String::New("unknown exception"));
        }
        return handle_scope.Close( scope->mongoToV8Element( ret.firstElement() ) );
    }

    Handle< Value > V8Scope::load( V8Scope* scope, const Arguments &args ) {
        Context::Scope context_scope(scope->_context);
        for (int i = 0; i < args.Length(); ++i) {
            std::string filename(toSTLString(args[i]));
            if (!scope->execFile(filename, false , true , false)) {
                return v8::ThrowException(v8::String::New((std::string("error loading file: ") + filename).c_str()));
            }
        }
        return v8::True();
    }

    /**
     * JS Callback that will call a c++ function with the v8 scope and v8 arguments.
     * Handles interrupts, exception handling, etc
     *
     * The implementation below assumes that SERVER-1816 has been fixed - in
     * particular, interrupted() must return true if an interrupt was ever
     * sent; currently that is not the case if a new killop overwrites the data
     * for an old one
     */
    v8::Handle< v8::Value > V8Scope::v8Callback( const v8::Arguments &args ) {
        disableV8Interrupt(); // we don't want to have to audit all v8 calls for termination exceptions, so we don't allow these exceptions during the callback
        if ( globalScriptEngine->interrupted() ) {
            v8::V8::TerminateExecution(); // experimentally it seems that TerminateExecution() will override the return value
            return v8::Undefined();
        }
        Local< External > f = External::Cast( *args.Callee()->Get( v8::String::New( "_v8_function" ) ) );
        v8Function function = (v8Function)(f->Value());
        Local< External > scp = External::Cast( *args.Data() );
        V8Scope* scope = (V8Scope*)(scp->Value());

        v8::Handle< v8::Value > ret;
        string exception;
        try {
            ret = function( scope, args );
        }
        catch( const std::exception &e ) {
            exception = e.what();
        }
        catch( ... ) {
            exception = "unknown exception";
        }
        enableV8Interrupt();
        if ( globalScriptEngine->interrupted() ) {
            v8::V8::TerminateExecution();
            return v8::Undefined();
        }
        if ( !exception.empty() ) {
            // technically, ThrowException is supposed to be the last v8 call before returning
            ret = v8::ThrowException( v8::String::New( exception.c_str() ) );
        }
        return ret;
    }

    // ---- global stuff ----

    void V8Scope::init( const BSONObj * data ) {
        V8Lock l;
        if ( ! data )
            return;

        BSONObjIterator i( *data );
        while ( i.more() ) {
            BSONElement e = i.next();
            setElement( e.fieldName() , e );
        }
    }

    void V8Scope::setNumber( const char * field , double val ) {
        V8_SIMPLE_HEADER
        _global->Set( getV8Str( field ) , v8::Number::New( val ) );
    }

    void V8Scope::setString( const char * field , const char * val ) {
        V8_SIMPLE_HEADER
        _global->Set( getV8Str( field ) , v8::String::New( val ) );
    }

    void V8Scope::setBoolean( const char * field , bool val ) {
        V8_SIMPLE_HEADER
        _global->Set( getV8Str( field ) , v8::Boolean::New( val ) );
    }

    void V8Scope::setElement( const char *field , const BSONElement& e ) {
        V8_SIMPLE_HEADER
        _global->Set( getV8Str( field ) , mongoToV8Element( e ) );
    }

    void V8Scope::setObject( const char *field , const BSONObj& obj , bool readOnly) {
        V8_SIMPLE_HEADER
        // Set() accepts a ReadOnly parameter, but this just prevents the field itself
        // from being overwritten and doesn't protect the object stored in 'field'.
        _global->Set( getV8Str( field ) , mongoToV8( obj, false, readOnly) );
    }

    int V8Scope::type( const char *field ) {
        V8_SIMPLE_HEADER
        Handle<Value> v = get( field );
        if ( v->IsNull() )
            return jstNULL;
        if ( v->IsUndefined() )
            return Undefined;
        if ( v->IsString() )
            return String;
        if ( v->IsFunction() )
            return Code;
        if ( v->IsArray() )
            return Array;
        if ( v->IsBoolean() )
            return Bool;
        if ( v->IsInt32() )
            return NumberInt;
        if ( v->IsNumber() )
            return NumberDouble;
        if ( v->IsExternal() ) {
            uassert( 10230 ,  "can't handle external yet" , 0 );
            return -1;
        }
        if ( v->IsDate() )
            return Date;
        if ( v->IsObject() )
            return Object;

        throw UserException( 12509, (string)"don't know what this is: " + field );
    }

    v8::Handle<v8::Value> V8Scope::get( const char * field ) {
        return _global->Get( getV8Str( field ) );
    }

    double V8Scope::getNumber( const char *field ) {
        V8_SIMPLE_HEADER
        return get( field )->ToNumber()->Value();
    }

    int V8Scope::getNumberInt( const char *field ) {
        V8_SIMPLE_HEADER
        return get( field )->ToInt32()->Value();
    }

    long long V8Scope::getNumberLongLong( const char *field ) {
        V8_SIMPLE_HEADER
        return get( field )->ToInteger()->Value();
    }

    string V8Scope::getString( const char *field ) {
        V8_SIMPLE_HEADER
        return toSTLString( get( field ) );
    }

    bool V8Scope::getBoolean( const char *field ) {
        V8_SIMPLE_HEADER
        return get( field )->ToBoolean()->Value();
    }

    BSONObj V8Scope::getObject( const char * field ) {
        V8_SIMPLE_HEADER
        Handle<Value> v = get( field );
        if ( v->IsNull() || v->IsUndefined() )
            return BSONObj();
        uassert( 10231 ,  "not an object" , v->IsObject() );
        return v8ToMongo( v->ToObject() );
    }

    // --- functions -----

    bool hasFunctionIdentifier( const string& code ) {
        if ( code.size() < 9 || code.find( "function" ) != 0  )
            return false;

        return code[8] == ' ' || code[8] == '(';
    }

    Local< v8::Function > V8Scope::__createFunction( const char * raw ) {
        raw = jsSkipWhiteSpace( raw );
        string code = raw;
        if ( !hasFunctionIdentifier( code ) ) {
            if ( code.find( "\n" ) == string::npos &&
                    ! hasJSReturn( code ) &&
                    ( code.find( ";" ) == string::npos || code.find( ";" ) == code.size() - 1 ) ) {
                code = "return " + code;
            }
            code = "function(){ " + code + "}";
        }

        int num = _funcs.size() + 1;

        string fn;
        {
            stringstream ss;
            ss << "_funcs" << num;
            fn = ss.str();
        }

        code = fn + " = " + code;

        TryCatch try_catch;
        // this might be time consuming, consider allowing an interrupt
        Handle<Script> script = v8::Script::Compile( v8::String::New( code.c_str() ) ,
                                v8::String::New( fn.c_str() ) );
        if ( script.IsEmpty() ) {
            _error = (string)"compile error: " + toSTLString( &try_catch );
            log() << _error << endl;
            return Local< v8::Function >();
        }

        Local<Value> result = script->Run();
        if ( result.IsEmpty() ) {
            _error = (string)"compile error: " + toSTLString( &try_catch );
            log() << _error << endl;
            return Local< v8::Function >();
        }

        return v8::Function::Cast( *_global->Get( v8::String::New( fn.c_str() ) ) );
    }

    ScriptingFunction V8Scope::_createFunction( const char * raw ) {
        V8_SIMPLE_HEADER
        Local< Value > ret = __createFunction( raw );
        if ( ret.IsEmpty() )
            return 0;
        Persistent<Value> f = Persistent< Value >::New( ret );
        uassert( 10232, "not a func" , f->IsFunction() );
        int num = _funcs.size() + 1;
        _funcs.push_back( f );
        return num;
    }

    void V8Scope::setThis( const BSONObj * obj ) {
        V8_SIMPLE_HEADER
        if ( ! obj ) {
            _this = Persistent< v8::Object >::New( v8::Object::New() );
            return;
        }

        //_this = mongoToV8( *obj );
        v8::Handle<v8::Value> argv[1];
        argv[0] = v8::External::New( createWrapperHolder( this, obj , true , false ) );
        _this = Persistent< v8::Object >::New( _wrapper->NewInstance( 1, argv ) );
    }

    void V8Scope::rename( const char * from , const char * to ) {
        V8_SIMPLE_HEADER;
        Handle<v8::String> f = getV8Str( from );
        Handle<v8::String> t = getV8Str( to );
        _global->Set( t , _global->Get( f ) );
        _global->Set( f , v8::Undefined() );
    }

    int V8Scope::invoke( ScriptingFunction func , const BSONObj& argsObject, int timeoutMs , bool ignoreReturn ) {
        V8_SIMPLE_HEADER
        Handle<Value> funcValue = _funcs[func-1];

        TryCatch try_catch;
        int nargs = argsObject.nFields();
        scoped_array< Handle<Value> > args;
        if ( nargs ) {
            args.reset( new Handle<Value>[nargs] );
            BSONObjIterator it( argsObject );
            for ( int i=0; i<nargs; i++ ) {
                BSONElement next = it.next();
                args[i] = mongoToV8Element( next );
            }
            setObject( "args", argsObject, true ); // for backwards compatibility
        }
        else {
            _global->Set( V8STR_ARGS, v8::Undefined() );
        }
        if ( globalScriptEngine->interrupted() ) {
            stringstream ss;
            ss << "error in invoke: " << globalScriptEngine->checkInterrupt();
            _error = ss.str();
            log() << _error << endl;
            return 1;
        }
        enableV8Interrupt(); // because of v8 locker we can check interrupted, then enable
        Local<Value> result = ((v8::Function*)(*funcValue))->Call( _this , nargs , args.get() );
        disableV8Interrupt();

        if ( result.IsEmpty() ) {
            stringstream ss;
            if ( try_catch.HasCaught() && !try_catch.CanContinue() ) {
                ss << "error in invoke: " << globalScriptEngine->checkInterrupt();
            }
            else {
                ss << "error in invoke: " << toSTLString( &try_catch );
            }
            _error = ss.str();
            log() << _error << endl;
            return 1;
        }

        if ( ! ignoreReturn ) {
            _global->Set( V8STR_RETURN , result );
        }

        return 0;
    }

    bool V8Scope::exec( const StringData& code , const string& name , bool printResult , bool reportError , bool assertOnError, int timeoutMs ) {
        if ( timeoutMs ) {
            static bool t = 1;
            if ( t ) {
                log() << "timeoutMs not support for v8 yet  code: " << code << endl;
                t = 0;
            }
        }

        V8_SIMPLE_HEADER

        TryCatch try_catch;

        Handle<Script> script = v8::Script::Compile( v8::String::New( code.data() ) ,
                                v8::String::New( name.c_str() ) );
        if (script.IsEmpty()) {
            stringstream ss;
            ss << "compile error: " << toSTLString( &try_catch );
            _error = ss.str();
            if (reportError)
                log() << _error << endl;
            if ( assertOnError )
                uassert( 10233 ,  _error , 0 );
            return false;
        }

        if ( globalScriptEngine->interrupted() ) {
            _error = (string)"exec error: " + globalScriptEngine->checkInterrupt();
            if ( reportError ) {
                log() << _error << endl;
            }
            if ( assertOnError ) {
                uassert( 13475 ,  _error , 0 );
            }
            return false;
        }
        enableV8Interrupt(); // because of v8 locker we can check interrupted, then enable
        Handle<v8::Value> result = script->Run();
        disableV8Interrupt();
        if ( result.IsEmpty() ) {
            if ( try_catch.HasCaught() && !try_catch.CanContinue() ) {
                _error = (string)"exec error: " + globalScriptEngine->checkInterrupt();
            }
            else {
                _error = (string)"exec error: " + toSTLString( &try_catch );
            }
            if ( reportError )
                log() << _error << endl;
            if ( assertOnError )
                uassert( 10234 ,  _error , 0 );
            return false;
        }

        _global->Set( getV8Str( "__lastres__" ) , result );

        if ( printResult && ! result->IsUndefined() ) {
            cout << toSTLString( result ) << endl;
        }

        return true;
    }

    void V8Scope::injectNative( const char *field, NativeFunction func ) {
        injectNative(field, func, _global);
    }

    void V8Scope::injectNative( const char *field, NativeFunction func, Handle<v8::Object>& obj ) {
        V8_SIMPLE_HEADER

        Handle< FunctionTemplate > ft = createV8Function(nativeCallback);
        ft->Set( this->V8STR_NATIVE_FUNC, External::New( (void*)func ) );
        obj->Set( getV8Str( field ), ft->GetFunction() );
    }

    void V8Scope::injectV8Function( const char *field, v8Function func ) {
        injectV8Function(field, func, _global);
    }

    void V8Scope::injectV8Function( const char *field, v8Function func, Handle<v8::Object>& obj ) {
        V8_SIMPLE_HEADER

        Handle< FunctionTemplate > ft = createV8Function(func);
        Handle<v8::Function> f = ft->GetFunction();
        obj->Set( getV8Str( field ), f );
    }

    void V8Scope::injectV8Function( const char *field, v8Function func, Handle<v8::Template>& t ) {
        V8_SIMPLE_HEADER

        Handle< FunctionTemplate > ft = createV8Function(func);
        Handle<v8::Function> f = ft->GetFunction();
        t->Set( getV8Str( field ), f );
    }

    Handle<FunctionTemplate> V8Scope::createV8Function( v8Function func ) {
        Handle< FunctionTemplate > ft = v8::FunctionTemplate::New(v8Callback, External::New( this ));
        ft->Set( this->V8STR_V8_FUNC, External::New( (void*)func ) );
        return ft;
    }

    void V8Scope::gc() {
        cout << "in gc" << endl;
        V8Lock l;
        while( !V8::IdleNotification() );
    }

    // ----- db access -----

    void V8Scope::localConnect( const char * dbName ) {
        {
            V8_SIMPLE_HEADER

            if ( _connectState == EXTERNAL )
                throw UserException( 12510, "externalSetup already called, can't call externalSetup" );
            if ( _connectState ==  LOCAL ) {
                if ( _localDBName == dbName )
                    return;
                throw UserException( 12511, "localConnect called with a different name previously" );
            }

            // needed for killop / interrupt support
            v8::Locker::StartPreemption( 50 );

            //_global->Set( v8::String::New( "Mongo" ) , _engine->_externalTemplate->GetFunction() );
            _global->Set( getV8Str( "Mongo" ) , getMongoFunctionTemplate( this, true )->GetFunction() );
            execCoreFiles();
            exec( "_mongo = new Mongo();" , "local connect 2" , false , true , true , 0 );
            exec( (string)"db = _mongo.getDB(\"" + dbName + "\");" , "local connect 3" , false , true , true , 0 );
            _connectState = LOCAL;
            _localDBName = dbName;
        }
        loadStored();
    }

    void V8Scope::externalSetup() {
        V8_SIMPLE_HEADER
        if ( _connectState == EXTERNAL )
            return;
        if ( _connectState == LOCAL )
            throw UserException( 12512, "localConnect already called, can't call externalSetup" );

        installFork( this, _global, _context );
        _global->Set( getV8Str( "Mongo" ) , getMongoFunctionTemplate( this, false )->GetFunction() );
        execCoreFiles();
        _connectState = EXTERNAL;
    }

    // ----- internal -----

    void V8Scope::reset() {
        _startCall();
    }

    void V8Scope::_startCall() {
        _error = "";
    }

    Handle<Value> NamedReadOnlySet( Local<v8::String> property, Local<Value> value, const AccessorInfo& info ) {
        cout << "cannot write to read-only object" << endl;
        return value;
    }

    Handle<Boolean> NamedReadOnlyDelete( Local<v8::String> property, const AccessorInfo& info ) {
        cout << "cannot delete from read-only object" << endl;
        return Boolean::New( false );
    }

    Handle<Value> IndexedReadOnlySet( uint32_t index, Local<Value> value, const AccessorInfo& info ) {
        cout << "cannot write to read-only array" << endl;
        return value;
    }

    Handle<Boolean> IndexedReadOnlyDelete( uint32_t index, const AccessorInfo& info ) {
        cout << "cannot delete from read-only array" << endl;
        return Boolean::New( false );
    }

    Local< v8::Value > newFunction( const char *code ) {
        stringstream codeSS;
        codeSS << "____MontoToV8_newFunction_temp = " << code;
        string codeStr = codeSS.str();
        Local< Script > compiled = Script::New( v8::String::New( codeStr.c_str() ) );
        Local< Value > ret = compiled->Run();
        return ret;
    }

    Local< v8::Value > V8Scope::newId( const OID &id ) {
        v8::Function * idCons = this->getObjectIdCons();
        v8::Handle<v8::Value> argv[1];
        argv[0] = v8::String::New( id.str().c_str() );
        return idCons->NewInstance( 1 , argv );
    }

    Local<v8::Object> V8Scope::mongoToV8( const BSONObj& m , bool array, bool readOnly ) {

        Local<v8::Object> o;

        // handle DBRef. needs to come first. isn't it? (metagoto)
        static string ref = "$ref";
        if ( ref == m.firstElement().fieldName() ) {
            const BSONElement& id = m["$id"];
            if (!id.eoo()) { // there's no check on $id exitence in sm implementation. risky ?
                v8::Function* dbRef = getNamedCons( "DBRef" );
                o = dbRef->NewInstance();
            }
        }

        Local< v8::ObjectTemplate > readOnlyObjects;
        // Hoping template construction is fast...
        Local< v8::ObjectTemplate > internalFieldObjects = v8::ObjectTemplate::New();
        internalFieldObjects->SetInternalFieldCount( 1 );

        if ( !o.IsEmpty() ) {
            readOnly = false;
        }
        else if ( array ) {
            // NOTE Looks like it's impossible to add interceptors to v8 arrays.
            readOnly = false;
            o = v8::Array::New();
        }
        else if ( !readOnly ) {
            o = v8::Object::New();
        }
        else {
            // NOTE Our readOnly implemention relies on undocumented ObjectTemplate
            // functionality that may be fragile, but it still seems like the best option
            // for now -- fwiw, the v8 docs are pretty sparse.  I've determined experimentally
            // that when property handlers are set for an object template, they will attach
            // to objects previously created by that template.  To get this to work, though,
            // it is necessary to initialize the template's property handlers before
            // creating objects from the template (as I have in the following few lines
            // of code).
            // NOTE In my first attempt, I configured the permanent property handlers before
            // constructiong the object and replaced the Set() calls below with ForceSet().
            // However, it turns out that ForceSet() only bypasses handlers for named
            // properties and not for indexed properties.
            readOnlyObjects = v8::ObjectTemplate::New();
            // NOTE This internal field will store type info for special db types.  For
            // regular objects the field is unnecessary - for simplicity I'm creating just
            // one readOnlyObjects template for objects where the field is & isn't necessary,
            // assuming that the overhead of an internal field is slight.
            readOnlyObjects->SetInternalFieldCount( 1 );
            readOnlyObjects->SetNamedPropertyHandler( 0 );
            readOnlyObjects->SetIndexedPropertyHandler( 0 );
            o = readOnlyObjects->NewInstance();
        }

        mongo::BSONObj sub;

        for ( BSONObjIterator i(m); i.more(); ) {
            const BSONElement& f = i.next();

            Local<Value> v;
            Handle<v8::String> name = getV8Str(f.fieldName());

            switch ( f.type() ) {

            case mongo::Code:
                o->Set( name, newFunction( f.valuestr() ) );
                break;

            case CodeWScope:
                if ( f.codeWScopeObject().isEmpty() )
                    log() << "warning: CodeWScope doesn't transfer to db.eval" << endl;
                o->Set( name, newFunction( f.codeWScopeCode() ) );
                break;

            case mongo::String:
                o->Set( name , v8::String::New( f.valuestr() ) );
                break;

            case mongo::jstOID: {
                v8::Function * idCons = getObjectIdCons();
                v8::Handle<v8::Value> argv[1];
                argv[0] = v8::String::New( f.__oid().str().c_str() );
                o->Set( name ,
                        idCons->NewInstance( 1 , argv ) );
                break;
            }

            case mongo::NumberDouble:
            case mongo::NumberInt:
                o->Set( name , v8::Number::New( f.number() ) );
                break;

            case mongo::Array:
            case mongo::Object:
                sub = f.embeddedObject();
                o->Set( name , mongoToV8( sub , f.type() == mongo::Array, readOnly ) );
                break;

            case mongo::Date:
                o->Set( name , v8::Date::New( f.date() ) );
                break;

            case mongo::Bool:
                o->Set( name , v8::Boolean::New( f.boolean() ) );
                break;

            case mongo::jstNULL:
            case mongo::Undefined: // duplicate sm behavior
                o->Set( name , v8::Null() );
                break;

            case mongo::RegEx: {
                v8::Function * regex = getNamedCons( "RegExp" );

                v8::Handle<v8::Value> argv[2];
                argv[0] = v8::String::New( f.regex() );
                argv[1] = v8::String::New( f.regexFlags() );

                o->Set( name , regex->NewInstance( 2 , argv ) );
                break;
            }

            case mongo::BinData: {
                Local<v8::Object> b = readOnly ? readOnlyObjects->NewInstance() : internalFieldObjects->NewInstance();

                int len;
                const char *data = f.binData( len );

                v8::Function* binData = getNamedCons( "BinData" );
                v8::Handle<v8::Value> argv[3];
                argv[0] = v8::Number::New( len );
                argv[1] = v8::Number::New( f.binDataType() );
                argv[2] = v8::String::New( data, len );
                o->Set( name, binData->NewInstance(3, argv) );
                break;
            }

            case mongo::Timestamp: {
                Local<v8::Object> sub = readOnly ? readOnlyObjects->NewInstance() : internalFieldObjects->NewInstance();

                sub->Set( V8STR_T , v8::Number::New( f.timestampTime() ) );
                sub->Set( V8STR_I , v8::Number::New( f.timestampInc() ) );
                sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );

                o->Set( name , sub );
                break;
            }

            case mongo::NumberLong: {
                Local<v8::Object> sub = readOnly ? readOnlyObjects->NewInstance() : internalFieldObjects->NewInstance();
                unsigned long long val = f.numberLong();
                v8::Function* numberLong = getNamedCons( "NumberLong" );
                double floatApprox = (double)(long long)val;
                if ( (long long)val == (long long)floatApprox ) {
                    v8::Handle<v8::Value> argv[1];
                    argv[0] = v8::Number::New( floatApprox );
                    o->Set( name, numberLong->NewInstance( 1, argv ) );
                }
                else {
                    v8::Handle<v8::Value> argv[3];
                    argv[0] = v8::Number::New( floatApprox );
                    argv[1] = v8::Integer::New( val >> 32 );
                    argv[2] = v8::Integer::New( (unsigned long)(val & 0x00000000ffffffff) );
                    o->Set( name, numberLong->NewInstance(3, argv) );
                }
                break;
            }

            case mongo::MinKey: {
                Local<v8::Object> sub = readOnly ? readOnlyObjects->NewInstance() : internalFieldObjects->NewInstance();
                sub->Set( V8STR_MINKEY, v8::Boolean::New( true ) );
                sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );
                o->Set( name , sub );
                break;
            }

            case mongo::MaxKey: {
                Local<v8::Object> sub = readOnly ? readOnlyObjects->NewInstance() : internalFieldObjects->NewInstance();
                sub->Set( V8STR_MAXKEY, v8::Boolean::New( true ) );
                sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );
                o->Set( name , sub );
                break;
            }

            case mongo::DBRef: {
                v8::Function* dbPointer = getNamedCons( "DBPointer" );
                v8::Handle<v8::Value> argv[2];
                argv[0] = getV8Str( f.dbrefNS() );
                argv[1] = newId( f.dbrefOID() );
                o->Set( name, dbPointer->NewInstance(2, argv) );
                break;
            }

            default:
                cout << "can't handle type: ";
                cout  << f.type() << " ";
                cout  << f.toString();
                cout  << endl;
                break;
            }

        }

        if ( readOnly ) {
            readOnlyObjects->SetNamedPropertyHandler( 0, NamedReadOnlySet, 0, NamedReadOnlyDelete );
            readOnlyObjects->SetIndexedPropertyHandler( 0, IndexedReadOnlySet, 0, IndexedReadOnlyDelete );
        }

        return o;
    }

    /**
     * converts a BSONObj to a Lazy V8 object
     */
    Local<v8::Object> V8Scope::mongoToLZV8( const BSONObj& m , bool array, bool readOnly ) {
        Local<v8::Object> o;

        if (array) {
            o = lzArrayTemplate->NewInstance();
            o->SetPrototype(v8::Array::New(1)->GetPrototype());
            o->Set(V8STR_LENGTH, v8::Integer::New(m.nFields()), DontEnum);
//            o->Set(ARRAY_STRING, v8::Boolean::New(true), DontEnum);
        } else {
            o = lzObjectTemplate->NewInstance();

            static string ref = "$ref";
            if ( ref == m.firstElement().fieldName() ) {
              const BSONElement& id = m["$id"];
              if (!id.eoo()) {
                  v8::Function* dbRef = getNamedCons( "DBRef" );
                  o->SetPrototype(dbRef->NewInstance()->GetPrototype());
              }
            }
        }

        BSONObj* p = new BSONObj(m);
        o->SetInternalField(0, v8::External::New(p));
        return o;
    }

    Handle<v8::Value> V8Scope::mongoToV8Element( const BSONElement &f, bool lazy ) {
        Local< v8::ObjectTemplate > internalFieldObjects = v8::ObjectTemplate::New();
        internalFieldObjects->SetInternalFieldCount( 1 );

        switch ( f.type() ) {

        case mongo::Code:
            return newFunction( f.valuestr() );

        case CodeWScope:
            if ( f.codeWScopeObject().isEmpty() )
                log() << "warning: CodeWScope doesn't transfer to db.eval" << endl;
            return newFunction( f.codeWScopeCode() );

        case mongo::String:
            return v8::String::New( f.valuestr() );

        case mongo::jstOID:
            return newId( f.__oid() );

        case mongo::NumberDouble:
        case mongo::NumberInt:
            return v8::Number::New( f.number() );

        case mongo::Array:
            // for arrays it's better to use non lazy object because:
            // - the lazy array is not a true v8 array and requires some v8 src change for all methods to work
            // - it made several tests about 1.5x slower
            // - most times when an array is accessed, all its values will be used
            return mongoToV8( f.embeddedObject() , f.type() == mongo::Array );
        case mongo::Object:
            if (lazy)
                return mongoToLZV8( f.embeddedObject() , f.type() == mongo::Array);
            return mongoToV8( f.embeddedObject() , f.type() == mongo::Array );

        case mongo::Date:
            return v8::Date::New( f.date() );

        case mongo::Bool:
            return v8::Boolean::New( f.boolean() );

        case mongo::EOO:
        case mongo::jstNULL:
        case mongo::Undefined: // duplicate sm behavior
            return v8::Null();

        case mongo::RegEx: {
            v8::Function * regex = getNamedCons( "RegExp" );

            v8::Handle<v8::Value> argv[2];
            argv[0] = v8::String::New( f.regex() );
            argv[1] = v8::String::New( f.regexFlags() );

            return regex->NewInstance( 2 , argv );
            break;
        }

        case mongo::BinData: {
            int len;
            const char *data = f.binData( len );

            v8::Function* binData = getNamedCons( "BinData" );
            v8::Handle<v8::Value> argv[3];
            argv[0] = v8::Number::New( len );
            argv[1] = v8::Number::New( f.binDataType() );
            argv[2] = v8::String::New( data, len );
            return binData->NewInstance( 3, argv );
        };

        case mongo::Timestamp: {
            Local<v8::Object> sub = internalFieldObjects->NewInstance();

            sub->Set( V8STR_T , v8::Number::New( f.timestampTime() ) );
            sub->Set( V8STR_I , v8::Number::New( f.timestampInc() ) );
            sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );

            return sub;
        }

        case mongo::NumberLong: {
            Local<v8::Object> sub = internalFieldObjects->NewInstance();
            unsigned long long val = f.numberLong();
            v8::Function* numberLong = getNamedCons( "NumberLong" );
            if ( (long long)val == (long long)(double)(long long)(val) ) {
                v8::Handle<v8::Value> argv[1];
                argv[0] = v8::Number::New( (double)(long long)( val ) );
                return numberLong->NewInstance( 1, argv );
            }
            else {
                v8::Handle<v8::Value> argv[3];
                argv[0] = v8::Number::New( (double)(long long)( val ) );
                argv[1] = v8::Integer::New( val >> 32 );
                argv[2] = v8::Integer::New( (unsigned long)(val & 0x00000000ffffffff) );
                return numberLong->NewInstance( 3, argv );
            }
        }

        case mongo::MinKey: {
            Local<v8::Object> sub = internalFieldObjects->NewInstance();
            sub->Set( V8STR_MINKEY, v8::Boolean::New( true ) );
            sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );
            return sub;
        }

        case mongo::MaxKey: {
            Local<v8::Object> sub = internalFieldObjects->NewInstance();
            sub->Set( V8STR_MAXKEY, v8::Boolean::New( true ) );
            sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );
            return sub;
        }

        case mongo::DBRef: {
            v8::Function* dbPointer = getNamedCons( "DBPointer" );
            v8::Handle<v8::Value> argv[2];
            argv[0] = getV8Str( f.dbrefNS() );
            argv[1] = newId( f.dbrefOID() );
            return dbPointer->NewInstance(2, argv);
        }

        default:
            cout << "can't handle type: ";
            cout  << f.type() << " ";
            cout  << f.toString();
            cout  << endl;
            break;
        }

        return v8::Undefined();
    }

    void V8Scope::v8ToMongoElement( BSONObjBuilder & b , v8::Handle<v8::String> name , const string sname , v8::Handle<v8::Value> value , int depth ) {

        if ( value->IsString() ) {
            b.append( sname , toSTLString( value ).c_str() );
            return;
        }

        if ( value->IsFunction() ) {
            b.appendCode( sname , toSTLString( value ) );
            return;
        }

        if ( value->IsNumber() ) {
            if ( value->IsInt32() )
                b.append( sname, int( value->ToInt32()->Value() ) );
            else
                b.append( sname , value->ToNumber()->Value() );
            return;
        }

        if ( value->IsArray() ) {
            BSONObj sub = v8ToMongo( value->ToObject() , depth );
            b.appendArray( sname , sub );
            return;
        }

        if ( value->IsDate() ) {
            b.appendDate( sname , Date_t( (unsigned long long)(v8::Date::Cast( *value )->NumberValue())) );
            return;
        }

        if ( value->IsExternal() )
            return;

        if ( value->IsObject() ) {
            // The user could potentially modify the fields of these special objects,
            // wreaking havoc when we attempt to reinterpret them.  Not doing any validation
            // for now...
            Local< v8::Object > obj = value->ToObject();
            if ( obj->InternalFieldCount() && obj->GetInternalField( 0 )->IsNumber() ) {
                switch( obj->GetInternalField( 0 )->ToInt32()->Value() ) { // NOTE Uint32's Value() gave me a linking error, so going with this instead
                case Timestamp:
                    b.appendTimestamp( sname,
                                       Date_t( (unsigned long long)(obj->Get( V8STR_T )->ToNumber()->Value() )),
                                       obj->Get( V8STR_I )->ToInt32()->Value() );
                    return;
                case MinKey:
                    b.appendMinKey( sname );
                    return;
                case MaxKey:
                    b.appendMaxKey( sname );
                    return;
                default:
                    assert( "invalid internal field" == 0 );
                }
            }
            string s = toSTLString( value );
            if ( s.size() && s[0] == '/' ) {
                s = s.substr( 1 );
                string r = s.substr( 0 , s.rfind( "/" ) );
                string o = s.substr( s.rfind( "/" ) + 1 );
                b.appendRegex( sname , r , o );
            }
            else if ( value->ToObject()->GetPrototype()->IsObject() &&
                      value->ToObject()->GetPrototype()->ToObject()->HasRealNamedProperty( V8STR_ISOBJECTID ) ) {
                OID oid;
                oid.init( toSTLString( value ) );
                b.appendOID( sname , &oid );
            }
            else if ( !value->ToObject()->GetHiddenValue( V8STR_NUMBERLONG ).IsEmpty() ) {
                // TODO might be nice to potentially speed this up with an indexed internal
                // field, but I don't yet know how to use an ObjectTemplate with a
                // constructor.
                v8::Handle< v8::Object > it = value->ToObject();
                long long val;
                if ( !it->Has( getV8Str( "top" ) ) ) {
                    val = (long long)( it->Get( getV8Str( "floatApprox" ) )->NumberValue() );
                }
                else {
                    val = (long long)
                          ( (unsigned long long)( it->Get( getV8Str( "top" ) )->ToInt32()->Value() ) << 32 ) +
                          (unsigned)( it->Get( getV8Str( "bottom" ) )->ToInt32()->Value() );
                }

                b.append( sname, val );
            }
            else if ( !value->ToObject()->GetHiddenValue( V8STR_DBPTR ).IsEmpty() ) {
                OID oid;
                oid.init( toSTLString( value->ToObject()->Get( getV8Str( "id" ) ) ) );
                string ns = toSTLString( value->ToObject()->Get( getV8Str( "ns" ) ) );
                b.appendDBRef( sname, ns, oid );
            }
            else if ( !value->ToObject()->GetHiddenValue( V8STR_BINDATA ).IsEmpty() ) {
                int len = obj->Get( getV8Str( "len" ) )->ToInt32()->Value();
                v8::String::Utf8Value data( obj->Get( getV8Str( "data" ) ) );
                const char *dataArray = *data;
                assert( data.length() == len );
                b.appendBinData( sname,
                                 len,
                                 mongo::BinDataType( obj->Get( getV8Str( "type" ) )->ToInt32()->Value() ),
                                 dataArray );
            }
            else {
                BSONObj sub = v8ToMongo( value->ToObject() , depth );
                b.append( sname , sub );
            }
            return;
        }

        if ( value->IsBoolean() ) {
            b.appendBool( sname , value->ToBoolean()->Value() );
            return;
        }

        else if ( value->IsUndefined() ) {
            b.appendUndefined( sname );
            return;
        }

        else if ( value->IsNull() ) {
            b.appendNull( sname );
            return;
        }

        cout << "don't know how to convert to mongo field [" << name << "]\t" << value << endl;
    }

    BSONObj V8Scope::v8ToMongo( v8::Handle<v8::Object> o , int depth ) {
        BSONObjBuilder b;

        if ( depth == 0 ) {
            if ( o->HasRealNamedProperty( V8STR_ID ) ) {
                v8ToMongoElement( b , V8STR_ID , "_id" , o->Get( V8STR_ID ) );
            }
        }

        Local<v8::Array> names = o->GetPropertyNames();
        for ( unsigned int i=0; i<names->Length(); i++ ) {
            v8::Local<v8::String> name = names->Get(v8::Integer::New(i) )->ToString();

            if ( o->GetPrototype()->IsObject() &&
                    o->GetPrototype()->ToObject()->HasRealNamedProperty( name ) )
                continue;

            v8::Local<v8::Value> value = o->Get( name );

            const string sname = toSTLString( name );
            if ( depth == 0 && sname == "_id" )
                continue;

            v8ToMongoElement( b , name , sname , value , depth + 1 );
        }
        return b.obj();
    }

    // --- random utils ----

    v8::Function * V8Scope::getNamedCons( const char * name ) {
        return v8::Function::Cast( *(v8::Context::GetCurrent()->Global()->Get( getV8Str( name ) ) ) );
    }

    v8::Function * V8Scope::getObjectIdCons() {
        return getNamedCons( "ObjectId" );
    }

    Handle<v8::Value> V8Scope::Print(V8Scope* scope, const Arguments& args) {
        bool first = true;
        for (int i = 0; i < args.Length(); i++) {
            HandleScope handle_scope;
            if (first) {
                first = false;
            }
            else {
                printf(" ");
            }
            v8::String::Utf8Value str(args[i]);
            printf("%s", *str);
        }
        printf("\n");
        return v8::Undefined();
    }

    Handle<v8::Value> V8Scope::Version(V8Scope* scope, const Arguments& args) {
        HandleScope handle_scope;
        return handle_scope.Close( v8::String::New(v8::V8::GetVersion()) );
    }

    Handle<v8::Value> V8Scope::GCV8(V8Scope* scope, const Arguments& args) {
        V8Lock l;
        while( !V8::IdleNotification() );
        return v8::Undefined();
    }

    /**
     * Gets a V8 strings from the scope's cache, creating one if needed
     */
    v8::Persistent<v8::String> V8Scope::getV8Str(string str) {
        Persistent<v8::String> ptr = _strCache[str];
        if (ptr.IsEmpty()) {
            ptr = Persistent<v8::String>::New(v8::String::New(str.c_str()));
            _strCache[str] = ptr;
//          cout << "Adding str " + str << endl;
        }
//      cout << "Returning str " + str << endl;
        return ptr;
    }

} // namespace mongo
