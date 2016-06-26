#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>

#include <Math.h>
#include "AudioFreezeEffect.h"
#include "CompileSwitches.h"

const float MIN_SPEED( 0.25f );
const float MAX_SPEED( 4.0f );

const int FILTER_SIZE( 3 );
const float gaussian_filter[] = { 0.25f, 0.5f, 0.25f };

/////////////////////////////////////////////////////////////////////

int freeze_queue_size_in_samples( int sample_size_in_bits )
{
  const int bytes_per_sample = sample_size_in_bits / 8;
  return FREEZE_QUEUE_SIZE_IN_BYTES / bytes_per_sample;
}

/////////////////////////////////////////////////////////////////////

AUDIO_FREEZE_EFFECT::AUDIO_FREEZE_EFFECT() :
  AudioStream( 1, m_input_queue_array ),
  m_buffer(),
  m_input_queue_array(),
  m_head(0),
  m_speed(1.0f),
  m_loop_start(0),
  m_loop_end(freeze_queue_size_in_samples(16) - 1),
  m_sample_size_in_bits(16),
  m_freeze_active(false),
  m_smooth_audio(false)
{
  memset( m_buffer, 0, sizeof(m_buffer) );
}

int16_t AUDIO_FREEZE_EFFECT::read_sample( int index ) const
{
  const int16_t* sample_buffer    = reinterpret_cast<const int16_t*>(m_buffer);

  if( m_smooth_audio )
  {
    float smoothed_sample( 0.0f );
    int start_index = index - ( FILTER_SIZE / 2 );
    if( start_index < m_loop_start )
    {
      start_index = m_loop_end - ( m_loop_start - start_index );
    }
    
    for( int x = 0; x < FILTER_SIZE; ++x )
    {
      int i = start_index + x;
      if( i >= m_loop_end )
      {
        i = m_loop_start;
      }


#ifdef DEBUG_OUTPUT
      Serial.print("loop start:");
      Serial.print(m_loop_start);
      Serial.print(" loop end:");
      Serial.print(m_loop_end);
      Serial.print(" i:");
      Serial.print(i);
      Serial.print("\n");
#endif // DEBUG_OUTPUT

      float s = sample_buffer[ i ];
      smoothed_sample += gaussian_filter[x] * s;
    }

    return roundf( smoothed_sample );
  }
  else
  {
    return sample_buffer[ index ];
  }
}

void AUDIO_FREEZE_EFFECT::write_to_buffer( const int16_t* source, int size )
{
  const int fqs                     = freeze_queue_size_in_samples( m_sample_size_in_bits );

  for( int x = 0; x < size; ++x )
  {
    // DO CONVERSION TO 8-bit HERE
    int16_t* sample_buffer            = reinterpret_cast<int16_t*>(m_buffer);
    sample_buffer[ trunc_to_int(m_head) ] = source[x];

    if( trunc_to_int(++m_head) == fqs )
    {
      m_head                        = 0.0f;
    }
  }
}

void AUDIO_FREEZE_EFFECT::read_from_buffer( int16_t* dest, int size )
{        
    for( int x = 0; x < size; ++x )
    {
      dest[x]                   = read_sample( trunc_to_int(m_head) );
      
      // head will have limited movement in freeze mode
      if( ++m_head >= m_loop_end )
      {
        m_head                  = m_loop_start;
      } 
    }
}

void AUDIO_FREEZE_EFFECT::read_from_buffer_with_speed( int16_t* dest, int size, float speed )
{          
    for( int x = 0; x < size; ++x )
    {
      if( speed < 1.0f )
      {
        int curr                = trunc_to_int(m_head);
        int next                = curr + 1;

        if( next >= m_loop_end )
        {
          next                  = m_loop_start;
        }

        int16_t sample          = lerp( read_sample(curr), read_sample(next), speed );
        dest[x]                 = sample;

        m_head                  += speed;

        if( m_head >= m_loop_end )
        {
          m_head                = m_loop_start;
        }
      }
      else
      {
        dest[x]                 = read_sample( trunc_to_int(m_head) );
        
        // head will have limited movement in freeze mode
        m_head                  += speed;
        if( m_head >= m_loop_end )
        {
          m_head                = m_loop_start;
        }        
      }
    }
}

void AUDIO_FREEZE_EFFECT::update()
{
  if( m_freeze_active )
  {
    audio_block_t* block        = allocate();

    if( block != nullptr )
    {
      //read_from_buffer( block->data, AUDIO_BLOCK_SAMPLES );
      read_from_buffer_with_speed( block->data, AUDIO_BLOCK_SAMPLES, m_speed );
  
      transmit( block, 0 );
    
      release( block );    
    }
  }
  else
  {
    audio_block_t* block        = receiveReadOnly();

    if( block != nullptr )
    {
      write_to_buffer( block->data, AUDIO_BLOCK_SAMPLES );
  
      transmit( block, 0 );
  
      release( block );
    }
  }
}

bool AUDIO_FREEZE_EFFECT::is_freeze_active() const
{
  return m_freeze_active; 
}

void AUDIO_FREEZE_EFFECT::set_freeze( bool active )
{
  m_freeze_active = active;  
}

void AUDIO_FREEZE_EFFECT::set_length( float length )
{
  const int loop_length = max_val<float>( roundf( length * freeze_queue_size_in_samples( m_sample_size_in_bits ) ), FILTER_SIZE );
  m_loop_end            = m_loop_start + loop_length;
}

void AUDIO_FREEZE_EFFECT::set_speed( float speed )
{
  if( speed < 0.5f )
  {
    // put in the range 0..1 
    float r = speed * 2.0f;
    m_speed = lerp( MIN_SPEED, 1.0f, r ); 
  }
  else
  {
    // put in the range 0..1
    float r = ( speed - 0.5f ) * 2.0f;
    m_speed = lerp( 1.0f, MAX_SPEED, r );    
  }
}

void AUDIO_FREEZE_EFFECT::set_centre( float centre )
{
  const int fqs     = freeze_queue_size_in_samples( m_sample_size_in_bits );
  int centre_index  = roundf( centre * fqs );

  int loop_length   = m_loop_end - m_loop_start;
  m_loop_start      = centre_index - loop_length;

  if( m_loop_start < 0 )
  {
    m_loop_start    = 0;
    m_loop_end      = loop_length - 1;
  }
  else if( m_loop_end > fqs - 1 )
  {
    m_loop_end      = fqs - 1;
    m_loop_start    = m_loop_end - loop_length;
  }
}

void AUDIO_FREEZE_EFFECT::set_smooth_audio( bool smooth )
{
  m_smooth_audio = smooth;
}


