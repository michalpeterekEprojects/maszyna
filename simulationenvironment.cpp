﻿/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

#include "stdafx.h"
#include "simulationenvironment.h"

#include "Globals.h"

namespace simulation {

world_environment Environment;

} // simulation

void
world_environment::toggle_daylight() {

    Global.FakeLight = !Global.FakeLight;

    if( Global.FakeLight ) {
        // for fake daylight enter fixed hour
        time( 10, 30, 0 );
    }
    else {
        // local clock based calculation
        time();
    }
}

// calculates current season of the year based on set simulation date
void
world_environment::compute_season( int const Yearday ) {

    using dayseasonpair = std::pair<int, std::string>;

    std::vector<dayseasonpair> seasonsequence {
        {  65, "winter:" },
        { 158, "spring:" },
        { 252, "summer:" },
        { 341, "autumn:" },
        { 366, "winter:" } };
    auto const lookup =
        std::lower_bound(
            std::begin( seasonsequence ), std::end( seasonsequence ),
            clamp( Yearday, 1, seasonsequence.back().first ),
            []( dayseasonpair const &Left, const int Right ) {
                return Left.first < Right; } );
    
    Global.Season = lookup->second;
    // season can affect the weather so if it changes, re-calculate weather as well
    compute_weather();
}

// calculates current weather
void
world_environment::compute_weather() {

    Global.Weather = (
        Global.Overcast <= 0.25 ? "clear:" :
        Global.Overcast <= 1.0 ? "cloudy:" :
        ( Global.Season != "winter:" ?
            "rain:" :
            "snow:" ) );

	m_precipitation.update_weather();
}

void
world_environment::init() {

    m_sun.init();
    m_moon.init();
    m_stars.init();
    m_clouds.Init();
    m_precipitation.init();

    m_precipitationsound.deserialize( "rain-sound-loop", sound_type::single );
}

void
world_environment::update() {
    // move celestial bodies...
    m_sun.update();
    m_moon.update();
    // ...determine source of key light and adjust global state accordingly...
    // diffuse (sun) intensity goes down after twilight, and reaches minimum 18 degrees below horizon
    float twilightfactor = clamp( -m_sun.getAngle(), 0.0f, 18.0f ) / 18.0f;
    // NOTE: sun light receives extra padding to prevent moon from kicking in too soon
    auto const sunlightlevel = m_sun.getIntensity() + 0.05f * ( 1.f - twilightfactor );
    auto const moonlightlevel = m_moon.getIntensity() * 0.65f; // scaled down by arbitrary factor, it's pretty bright otherwise

    // ...update skydome to match the current sun position as well...
    // twilight factor can be reset later down, so we do it here while it's still reflecting state of the sun
    // turbidity varies from 2-3 during the day based on overcast, 3-4 after sunset to deal with sunlight bleeding too much into the sky from below horizon
    m_skydome.SetTurbidity(
        2.f
        + clamp( Global.Overcast, 0.f, 1.f )
        + interpolate( 0.f, 1.f, clamp( twilightfactor * 1.5f, 0.f, 1.f ) ) );
    m_skydome.SetOvercastFactor( Global.Overcast );
    m_skydome.Update( m_sun.getDirection() );

    float keylightintensity;
    glm::vec3 keylightcolor;
    if( moonlightlevel > sunlightlevel ) {
        // rare situations when the moon is brighter than the sun, typically at night
        Global.SunAngle = m_moon.getAngle();
        Global.DayLight.position = m_moon.getDirection();
        Global.DayLight.direction = -1.0f * m_moon.getDirection();
        keylightintensity = moonlightlevel;
        // if the moon is up, it overrides the twilight
        twilightfactor = 0.0f;
        keylightcolor = glm::vec3( 255.0f / 255.0f, 242.0f / 255.0f, 202.0f / 255.0f );
    }
    else {
        // regular situation with sun as the key light
        Global.SunAngle = m_sun.getAngle();
        Global.DayLight.position = m_sun.getDirection();
        Global.DayLight.direction = -1.0f * m_sun.getDirection();
        keylightintensity = sunlightlevel;
        // include 'golden hour' effect in twilight lighting
        float const duskfactor = 1.0f - clamp( Global.SunAngle, 0.0f, 18.0f ) / 18.0f;
        keylightcolor = interpolate(
            glm::vec3( 255.0f / 255.0f, 242.0f / 255.0f, 231.0f / 255.0f ),
            glm::vec3( 235.0f / 255.0f, 140.0f / 255.0f, 36.0f / 255.0f ),
            duskfactor );
    }
    // ...retrieve current sky colour and brightness...
    auto const skydomecolour = m_skydome.GetAverageColor();
    auto const skydomehsv = colors::RGBtoHSV( skydomecolour );
    // sun strength is reduced by overcast level
    keylightintensity *= ( 1.0f - std::min( 1.f, Global.Overcast ) * 0.65f );

    // intensity combines intensity of the sun and the light reflected by the sky dome
    // it'd be more technically correct to have just the intensity of the sun here,
    // but whether it'd _look_ better is something to be tested
    auto const intensity = std::min( 1.15f * ( 0.05f + keylightintensity + skydomehsv.z ), 1.25f );
    // the impact of sun component is reduced proportionally to overcast level, as overcast increases role of ambient light
    auto const diffuselevel = interpolate( keylightintensity, intensity * ( 1.0f - twilightfactor ), 1.0f - std::min( 1.f, Global.Overcast ) * 0.75f );
    // ...update light colours and intensity.
    keylightcolor = keylightcolor * diffuselevel;
    Global.DayLight.diffuse = glm::vec4( keylightcolor, Global.DayLight.diffuse.a );
    Global.DayLight.specular = glm::vec4( keylightcolor * 0.85f, diffuselevel );

    // tonal impact of skydome color is inversely proportional to how high the sun is above the horizon
    // (this is pure conjecture, aimed more to 'look right' than be accurate)
    float const ambienttone = clamp( 1.0f - ( Global.SunAngle / 90.0f ), 0.0f, 1.0f );
    Global.DayLight.ambient[ 0 ] = interpolate( skydomehsv.z, skydomecolour.r, ambienttone );
    Global.DayLight.ambient[ 1 ] = interpolate( skydomehsv.z, skydomecolour.g, ambienttone );
    Global.DayLight.ambient[ 2 ] = interpolate( skydomehsv.z, skydomecolour.b, ambienttone );

    Global.fLuminance = intensity;

    // update the fog. setting it to match the average colour of the sky dome is cheap
    // but quite effective way to make the distant items blend with background better
    // NOTE: base brightness calculation provides scaled up value, so we bring it back to 'real' one here
    Global.FogColor = m_skydome.GetAverageHorizonColor();

    // weather-related simulation factors
    // TODO: dynamic change of air temperature and overcast levels
    if( Global.Weather == "rain:" ) {
        // reduce friction in rain
        Global.FrictionWeatherFactor = 0.85f;
        m_precipitationsound.play( sound_flags::exclusive | sound_flags::looping );
    }
    else if( Global.Weather == "snow:" ) {
        // reduce friction due to snow
        Global.FrictionWeatherFactor = 0.75f;
		m_precipitationsound.stop();
    }
	else {
		m_precipitationsound.stop();
	}
}

void
world_environment::update_precipitation() {

    m_precipitation.update();
}

void
world_environment::time( int const Hour, int const Minute, int const Second ) {

    m_sun.setTime( Hour, Minute, Second );
}

//---------------------------------------------------------------------------
