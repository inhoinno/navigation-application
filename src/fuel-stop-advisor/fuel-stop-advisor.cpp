#include <dbus-c++/glib-integration.h>
#include <glib.h>
#include <stdio.h>
#include <unistd.h>
#include "fuel-stop-advisor.h"
#include "constants.h"
#include "amb.h"
#include "ctripcomputer.h"
#include <boost/variant/get.hpp>
#include "genivi-navigationcore-constants.h"
#include "genivi-navigationcore-routing_proxy.h"

static DBus::Glib::BusDispatcher dispatcher;
static DBus::Connection *conn;
static class FuelStopAdvisor *server;
static GMainLoop *loop;

#define dbgprintf(...) realdbgprintf (__FILE__, __LINE__, __VA_ARGS__)

#if (!DEBUG_ENABLED)
#undef dbgprintf
#define dbgprintf(...) ;
#endif


/* vehicle parameter */
static double fuel_consumption_l_100km=6.3;

static double fuel_consumption_speed[]={
	6.0, /* 0-9 km/h */
	6.0, /* 10-19 km/h */
	6.0, /* 20-29 km/h */
	6.0, /* 30-39 km/h */
	4.5, /* 40-49 km/h */
	4.5, /* 50-59 km/h */
	4.5, /* 60-69 km/h */
	5.5, /* 70-79 km/h */
	5.5, /* 80-89 km/h */
	7.0, /* 90-99 km/h */
	7.0, /* 100-109 km/h */
	7.0, /* 110-119 km/h */
	7.0, /* 120-129 km/h */
};


static DBus::Variant
variant_uint16(uint16_t i)
{
	DBus::Variant variant;
	DBus::MessageIter iter=variant.writer();
	iter << i;
	return variant;
}

static DBus::Variant
variant_uint32(uint32_t i)
{
	DBus::Variant variant;
	DBus::MessageIter iter=variant.writer();
	iter << i;
	return variant;
}


class Properties
: public ::DBus::InterfaceProxy,
  public ::DBus::ObjectProxy
{
	public:

	Properties(::DBus::Connection &connection, ::DBus::Path path)
        : ::DBus::InterfaceProxy("org.freedesktop.DBus.Properties"),
        ::DBus::ObjectProxy(connection, path,"org.automotive.message.broker")
	{
	}
	::DBus::Variant Get(const std::string &iface, const std::string &property)
	{
		::DBus::CallMessage call;
		::DBus::MessageIter wi = call.writer();
		
		wi << iface;
		wi << property;
		call.member("Get");
		::DBus::Message ret = invoke_method (call);
		::DBus::MessageIter ri = ret.reader();
		::DBus::Variant argout;
		ri >> argout;
		return argout;
	}
};

class Routing
: public org::genivi::navigationcore::Routing_proxy,
  public DBus::ObjectProxy
{
	public:
	Routing(DBus::Connection &connection)
        : DBus::ObjectProxy(connection, "/org/genivi/navigationcore","org.genivi.navigationcore.Routing")
	{
	}

	void RouteDeleted(const uint32_t& routeHandle)
	{
	}

	void RouteCalculationCancelled(const uint32_t& routeHandle)
	{
	}

	void RouteCalculationSuccessful(const uint32_t& routeHandle, const std::map< uint16_t, uint16_t >& unfullfilledPreferences)
	{
	}

	void RouteCalculationFailed(const uint32_t& routeHandle, const uint16_t& errorCode, const std::map< uint16_t, uint16_t >& unfullfilledPreferences)
	{
	}

	void RouteCalculationProgressUpdate(const uint32_t& routeHandle, const uint16_t& status, const uint8_t& percentage)
	{
	}

	void AlternativeRoutesAvailable(const std::vector< uint32_t >& routeHandlesList)
	{
	}
};


class AutomotiveMessageBroker
: public org::automotive::Manager_proxy,
  public DBus::ObjectProxy
{
	public:

	AutomotiveMessageBroker(DBus::Connection &connection)
        : DBus::ObjectProxy(connection, "/","org.automotive.message.broker")
	{
	}
};


class FuelStopAdvisor
: public org::genivi::demonstrator::FuelStopAdvisor_adaptor,
  public DBus::IntrospectableAdaptor,
  public DBus::ObjectAdaptor
{
	public:
        FuelStopAdvisor(DBus::Connection &connection)
        : DBus::ObjectAdaptor(connection, "/org/genivi/demonstrator/FuelStopAdvisor")
        {

        //connect to amb and create two handlers for fuel and odometer
		amb_dispatcher.attach(NULL);
		amb_conn = new DBus::Connection(DBus::Connection::SessionBus());
		amb_conn->setup(&amb_dispatcher);
		amb=new AutomotiveMessageBroker(*amb_conn);

		routing_dispatcher.attach(NULL);
		routing_conn = new DBus::Connection(DBus::Connection::SessionBus());
		routing_conn->setup(&routing_dispatcher);
		routing=new Routing(*routing_conn);

		fuel = amb->FindObject("Fuel");
		odometer = amb->FindObject("Odometer");

		fuel_properties=new Properties(*amb_conn,fuel[0]);
		odometer_properties=new Properties(*amb_conn,odometer[0]);

        // create an instance of basic trip computer and initialize it
        mp_tripComputer = new CTripComputer();

        mp_tripComputer->Initialize(CTripComputer::INSTANT_FUEL_CONSUMPTION_START_VALUE);

        tripComputerInput_t tripComputerInput;

        tripComputerInput.fuelLevel = 0;
        tripComputerInput.time = 0;
        tripComputerInput.distance = 0;
        tripComputerInput.fuelConsumption = 0;

        mp_tripComputer->RefreshTripComputerInput(tripComputerInput);

        for (uint8_t i; i<mp_tripComputer->GetSupportedTripNumbers();i++)
        {
            mp_tripComputer->ResetTrip(i);
        }

        // init fsa settings
		advisorMode=false;
		distanceThreshold=0;
        destinationCantBeReached=false;
		routeHandle=0;
        initFlag=true;
        }

	::DBus::Struct< uint16_t, uint16_t, uint16_t, std::string >
	GetVersion()
	{
        ::DBus::Struct< uint16_t, uint16_t, uint16_t, std::string > ret;
        version_t version;
        version = mp_tripComputer->GetVersion();  //Get the version of the basic trip computer
        ret._1=version.major;
        ret._2=version.minor;
        ret._3=version.micro;
        ret._4=version.date;
        return ret;
    }

	void
	SetUnits(const std::map< uint16_t, uint16_t >& data)
	{
        mp_tripComputer->SetUnits(data); //Set units of the basic trip computer
    }

	std::map< uint16_t, ::DBus::Variant >
    GetInstantData()
	{
		std::map< uint16_t, ::DBus::Variant > ret;
        tupleVariantTripComputer_t tripComputerData;
        tupleVariantTripComputer_t::iterator iter;
        variantTripComputer_t value;
        uint16_t level;
        double remaining;

        tripComputerData= mp_tripComputer->GetInstantData();

        for(iter = tripComputerData.begin(); iter != tripComputerData.end(); iter++)
        {
            value = iter->second;
            if (iter->first == CTripComputer::TRIPCOMPUTER_INSTANT_FUEL_CONSUMPTION_PER_DISTANCE)
            {
                ret[GENIVI_FUELSTOPADVISOR_INSTANT_FUEL_CONSUMPTION_PER_DISTANCE]=variant_uint16(boost::get<uint16_t>(iter->second));
            }
            else
            {
                if (iter->first == CTripComputer::TRIPCOMPUTER_TANK_DISTANCE)
                { //tank distance is valid, so it means that fuel level is valid too
                    ret[GENIVI_FUELSTOPADVISOR_TANK_DISTANCE]=variant_uint16(boost::get<uint16_t>(iter->second));
                    ret[GENIVI_FUELSTOPADVISOR_FUEL_LEVEL]=variant_uint16(fuelLevel);
                    if (this->routeHandle != 0)
                    { // a route is valid so it makes sense to calculate enhanced tank distance
                        ret[GENIVI_FUELSTOPADVISOR_ENHANCED_TANK_DISTANCE]=variant_uint16(enhancedDistance(fuelLevel,remaining)+0.5);
                    }
                }
            }
        }

        return ret;
	}

	uint8_t
	GetSupportedTripNumbers()
	{
        return (mp_tripComputer->GetSupportedTripNumbers());
    }

	std::map< uint16_t, ::DBus::Variant >
	GetTripData(const uint8_t& number)
	{
        std::map< uint16_t, ::DBus::Variant > ret;
        tupleVariantTripComputer_t tripComputerData;
        tupleVariantTripComputer_t::iterator iter;
        variantTripComputer_t value;

        if (number >= mp_tripComputer->GetSupportedTripNumbers())
            throw DBus::ErrorInvalidArgs("Invalid trip number");

        tripComputerData= mp_tripComputer->GetTripData(number);
        for(iter = tripComputerData.begin(); iter != tripComputerData.end(); iter++)
        {
            value = iter->second;
            if (iter->first == CTripComputer::TRIPCOMPUTER_AVERAGE_FUEL_CONSUMPTION_PER_DISTANCE)
            {
                ret[GENIVI_FUELSTOPADVISOR_AVERAGE_FUEL_CONSUMPTION_PER_DISTANCE]=variant_uint16(boost::get<uint16_t>(iter->second));
            }
            else
            {
                if (iter->first == CTripComputer::TRIPCOMPUTER_AVERAGE_SPEED)
                {
                    ret[GENIVI_FUELSTOPADVISOR_AVERAGE_SPEED]=variant_uint16(boost::get<uint16_t>(iter->second));
                }
                else
                {
                    if (iter->first == CTripComputer::TRIPCOMPUTER_DISTANCE)
                    {
                        ret[GENIVI_FUELSTOPADVISOR_DISTANCE]=variant_uint16(boost::get<uint16_t>(iter->second));
                    }
                }
            }
        }
        return ret;
    }

	void ResetTripData(const uint8_t& number)
	{
        if (number >= mp_tripComputer->GetSupportedTripNumbers())
           throw DBus::ErrorInvalidArgs("Invalid trip number");
        mp_tripComputer->ResetTrip(number);
		TripDataResetted(number);
    }

	double fuelConsumptionAtSpeed(uint16_t seg_speed)
	{
		if (seg_speed > 129)
			seg_speed=129;
		return fuel_consumption_speed[seg_speed/10];
	}

	double enhancedDistance(double level, double &remaining)
	{
		double distance=0;
        dbgprintf("routeHandle %d\n",routeHandle);
		if (routeHandle) {
			std::vector< std::map< uint16_t, ::DBus::Variant > > RouteShape;
			std::vector< uint16_t > valuesToReturn;
			uint32_t totalNumberOfSegments;
			valuesToReturn.push_back(GENIVI_NAVIGATIONCORE_DISTANCE);
			valuesToReturn.push_back(GENIVI_NAVIGATIONCORE_SPEED);
			routing->GetRouteSegments(routeHandle, 1, valuesToReturn, 0xffffffff, 0, totalNumberOfSegments, RouteShape);
			for (int i=0 ; i < RouteShape.size(); i++) {
				double seg_distance;
				uint16_t seg_speed;
				DBus::MessageIter it;

				it=RouteShape[i][GENIVI_NAVIGATIONCORE_DISTANCE].reader();
				it >> seg_distance;
				it=RouteShape[i][GENIVI_NAVIGATIONCORE_SPEED].reader();
				it >> seg_speed;
				if (seg_distance && seg_speed) {
					double fuel_consumption=fuelConsumptionAtSpeed(seg_speed)*seg_distance/100000;
					if (fuel_consumption > level && level > 0) {
						seg_distance=seg_distance*level/fuel_consumption;
						fuel_consumption=level;
					}
					if (level > 0) 
						distance+=seg_distance;
					level-=fuel_consumption;
				}
			}
            dbgprintf("%d segments\n",totalNumberOfSegments);
		}
		remaining=level/fuel_consumption_l_100km*100;
        dbgprintf("distance_on_route %f remaining %f\n",distance/1000,remaining);
		return distance/1000+(remaining > 0 ? remaining:0);
	}
	
	void update_data()
	{
        tripComputerInput_t tripComputerInput;
        DBus::Variant variant;
        DBus::MessageIter it;
        uint32_t odometer;
        uint16_t level;
        uint16_t consumption;
        double time;


        variant=fuel_properties->Get("org.automotive.Fuel","Level");
        it=variant.reader();
        it >> level;

        variant=odometer_properties->Get("org.automotive.Odometer","Time");
        it=variant.reader();
        it >> time;

        //odometer is used for simulating distance
        variant=odometer_properties->Get("org.automotive.Odometer","Odometer");
        it=variant.reader();
        it >> odometer;

        if (initFlag)
        {
            if (time != 0)
            {
                initFlag = false;
                lastTime = time;
                lastOdometer = odometer;
            }
            timeCounter = 0;
            distanceCounter = 0;
        }
        else
        {
            if ((timeCounter+(time-lastTime)*CTripComputer::CONVERT_SECOND_IN_MILLISECOND) > USHRT_MAX)
            {
                timeCounter =  (time-lastTime)*CTripComputer::CONVERT_SECOND_IN_MILLISECOND - (USHRT_MAX - timeCounter);
            }
            else
            {
                timeCounter += (time-lastTime)*CTripComputer::CONVERT_SECOND_IN_MILLISECOND;
            }
            lastTime = time;

//            if ((distanceCounter+(odometer-lastOdometer)) > USHRT_MAX)
            if ((distanceCounter+odometer) > USHRT_MAX)
            {
//                distanceCounter =  (odometer-lastOdometer) - (USHRT_MAX - distanceCounter);
                distanceCounter =  (distanceCounter+odometer)-USHRT_MAX ;
            }
            else
            {
//                distanceCounter += (odometer-lastOdometer);
                distanceCounter += odometer;
            }
//            lastOdometer = odometer;

        }
        tripComputerInput.time = timeCounter;
        tripComputerInput.distance = distanceCounter;

        variant=fuel_properties->Get("org.automotive.Fuel","InstantConsumption");
        it=variant.reader();
        it >> consumption;

        tripComputerInput.fuelLevel = level*CTripComputer::CONVERT_LITER_IN_DL;
        tripComputerInput.fuelConsumption = consumption;

        mp_tripComputer->RefreshTripComputerInput(tripComputerInput);

        fuelLevel = level; //to avoid re-ask it to amb

        TripDataUpdated(0); //arg is for future use
	}

    void updateEnhancedDistance()
    {
        double remaining;
        if (advisorMode) {
            enhancedDistance(fuelLevel, remaining);
            dbgprintf("Advisor %f vs %d\n",remaining, distanceThreshold);
            if (remaining < distanceThreshold) {
                dbgprintf("Warning %f < %d\n",remaining, distanceThreshold);
                destinationCantBeReached = true;
            }
            else
            {
                destinationCantBeReached = false;
            }
            FuelStopAdvisorWarning(destinationCantBeReached);
            TripDataUpdated(0); //arg is for future use
        }
    }

	void
	SetFuelAdvisorSettings(const bool& advisorMode, const uint8_t& distanceThreshold)
	{
        dbgprintf("SetFuelAdvisorSettings(%d,%d)\n",advisorMode, distanceThreshold);
		this->advisorMode=advisorMode;
		this->distanceThreshold=distanceThreshold;
        updateEnhancedDistance();
	}
	
	void
    GetFuelAdvisorSettings(bool& advisorMode, uint8_t& distanceThreshold, bool& destinationCantBeReached)
	{
		advisorMode=this->advisorMode;
		distanceThreshold=this->distanceThreshold;
        destinationCantBeReached=this->destinationCantBeReached;
	}

	void SetRouteHandle(const uint32_t& routeHandle)
	{
        dbgprintf("SetRouteHandle %d\n",routeHandle);
		this->routeHandle=routeHandle;
        updateEnhancedDistance();
	}

    void ReleaseRouteHandle(const uint32_t& routeHandle)
    {
        dbgprintf("ResetRouteHandle %d\n",routeHandle);
        this->routeHandle=0;
        updateEnhancedDistance();
    }

	private:
	DBus::Glib::BusDispatcher amb_dispatcher;
	DBus::Connection *amb_conn;
	AutomotiveMessageBroker *amb;
	DBus::Glib::BusDispatcher routing_dispatcher;
	DBus::Connection *routing_conn;
	Routing *routing;
	std::vector< ::DBus::Path > fuel;
	std::vector< ::DBus::Path > odometer;
	Properties *fuel_properties;
	Properties *odometer_properties;
    CTripComputer *mp_tripComputer;
	bool advisorMode;
	uint8_t distanceThreshold;
    bool destinationCantBeReached;
	uint32_t routeHandle;
    bool initFlag;
    double lastTime;
    uint16_t timeCounter;
    uint32_t lastOdometer;
    uint16_t distanceCounter;
    uint16_t fuelLevel;
};

static gboolean
update_data(gpointer user_data)
{
	FuelStopAdvisor *tc=(FuelStopAdvisor *)user_data;
	tc->update_data();
	return TRUE;
}

int main(int argc, char **argv)
{
	loop=g_main_loop_new(NULL, false);
	dispatcher.attach(NULL);
	DBus::default_dispatcher = &dispatcher;
	conn = new DBus::Connection(DBus::Connection::SessionBus());
	conn->setup(&dispatcher);
	conn->request_name("org.genivi.demonstrator.FuelStopAdvisor");
	server=new FuelStopAdvisor(*conn);

    g_timeout_add(CTripComputer::SAMPLING_TIME*CTripComputer::CONVERT_SECOND_IN_MILLISECOND, update_data, server);
	g_main_loop_run(loop);
}
