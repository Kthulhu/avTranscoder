#include "OutputFile.hpp"

#include <AvTranscoder/util.hpp>

#include <stdexcept>

#ifndef FF_INPUT_BUFFER_PADDING_SIZE
 #define FF_INPUT_BUFFER_PADDING_SIZE 16
#endif

namespace avtranscoder
{

OutputFile::OutputFile( const std::string& filename, const std::string& formatName, const std::string& mimeType )
	: _formatContext( AV_OPT_FLAG_ENCODING_PARAM )
	, _outputStreams()
	, _frameCount()
	, _previousProcessedStreamDuration( 0.0 )
	, _profile()
{
	_formatContext.setFilename( filename );
	_formatContext.setOutputFormat( filename, formatName, mimeType );
}

OutputFile::~OutputFile()
{
	for( std::vector< OutputStream* >::iterator it = _outputStreams.begin(); it != _outputStreams.end(); ++it )
	{
		delete (*it);
	}
}

IOutputStream& OutputFile::addVideoStream( const VideoCodec& videoDesc )
{
	AVStream& stream = _formatContext.addAVStream( videoDesc.getAVCodec() );

	stream.codec->width  = videoDesc.getAVCodecContext().width;
	stream.codec->height = videoDesc.getAVCodecContext().height;
	stream.codec->bit_rate = videoDesc.getAVCodecContext().bit_rate;
	stream.codec->pix_fmt = videoDesc.getAVCodecContext().pix_fmt;
	stream.codec->profile = videoDesc.getAVCodecContext().profile;
	stream.codec->level = videoDesc.getAVCodecContext().level;

	// some codecs need/can use extradata to decode
	uint8_t* srcExtradata = videoDesc.getAVCodecContext().extradata;
	const int srcExtradataSize = videoDesc.getAVCodecContext().extradata_size;
	stream.codec->extradata = (uint8_t*) av_malloc( srcExtradataSize + FF_INPUT_BUFFER_PADDING_SIZE );
	memcpy( stream.codec->extradata, srcExtradata, srcExtradataSize );
	memset( ((uint8_t *) stream.codec->extradata) + srcExtradataSize, 0, FF_INPUT_BUFFER_PADDING_SIZE );
	stream.codec->extradata_size = videoDesc.getAVCodecContext().extradata_size;

	// need to set the time_base on the AVCodecContext and the AVStream
	// compensating the frame rate with the ticks_per_frame and keeping
	// a coherent reading speed.
	av_reduce(
		&stream.codec->time_base.num,
		&stream.codec->time_base.den,
		videoDesc.getAVCodecContext().time_base.num * videoDesc.getAVCodecContext().ticks_per_frame,
		videoDesc.getAVCodecContext().time_base.den,
		INT_MAX );

	stream.time_base = stream.codec->time_base;

	OutputStream* outputStream = new OutputStream( *this, _formatContext.getNbStreams() - 1 );
	_outputStreams.push_back( outputStream );

	return *outputStream;
}

IOutputStream& OutputFile::addAudioStream( const AudioCodec& audioDesc )
{
	AVStream& stream = _formatContext.addAVStream( audioDesc.getAVCodec() );

	stream.codec->sample_rate = audioDesc.getAVCodecContext().sample_rate;
	stream.codec->channels = audioDesc.getAVCodecContext().channels;
	stream.codec->channel_layout = audioDesc.getAVCodecContext().channel_layout;
	stream.codec->sample_fmt = audioDesc.getAVCodecContext().sample_fmt;
	stream.codec->frame_size = audioDesc.getAVCodecContext().frame_size;

	// need to set the time_base on the AVCodecContext of the AVStream
	av_reduce(
		&stream.codec->time_base.num,
		&stream.codec->time_base.den,
		audioDesc.getAVCodecContext().time_base.num,
		audioDesc.getAVCodecContext().time_base.den,
		INT_MAX );

	OutputStream* outputStream = new OutputStream( *this, _formatContext.getNbStreams() - 1 );
	_outputStreams.push_back( outputStream );

	return *outputStream;
}

IOutputStream& OutputFile::addDataStream( const DataCodec& dataDesc )
{
	_formatContext.addAVStream( dataDesc.getAVCodec() );

	OutputStream* outputStream = new OutputStream( *this, _formatContext.getNbStreams() - 1 );
	_outputStreams.push_back( outputStream );

	return *outputStream;
}

IOutputStream& OutputFile::getStream( const size_t streamIndex )
{
	if( streamIndex >= _outputStreams.size() )
		throw std::runtime_error( "unable to get output stream (out of range)" );
	return *_outputStreams.at( streamIndex );
}

std::string OutputFile::getFilename() const
{
	return std::string( _formatContext.getAVFormatContext().filename );
}

std::string OutputFile::getFormatName() const
{
	if( _formatContext.getAVOutputFormat().name == NULL )
	{
		LOG_WARN("Unknown muxer format name of '" << getFilename() << "'.")
		return "";
	}
	return std::string(_formatContext.getAVOutputFormat().name);
}

std::string OutputFile::getFormatLongName() const
{
	if( _formatContext.getAVOutputFormat().long_name == NULL )
	{
		LOG_WARN("Unknown muxer format long name of '" << getFilename() << "'.")
		return "";
	}
	return std::string(_formatContext.getAVOutputFormat().long_name);
}

std::string OutputFile::getFormatMimeType() const
{
	if( _formatContext.getAVOutputFormat().mime_type == NULL )
	{
		LOG_WARN("Unknown muxer format mime type of '" << getFilename() << "'.")
		return "";
	}
	return std::string(_formatContext.getAVOutputFormat().mime_type);
}

bool OutputFile::beginWrap( )
{
	LOG_DEBUG( "Begin wrap of OutputFile" )

	_formatContext.openRessource( getFilename(), AVIO_FLAG_WRITE );
	_formatContext.writeHeader();

	// set specific wrapping options
	setupRemainingWrappingOptions();

	_frameCount.clear();
	_frameCount.resize( _outputStreams.size(), 0 );

	return true;
}

IOutputStream::EWrappingStatus OutputFile::wrap( const CodedData& data, const size_t streamIndex )
{
	if( ! data.getSize() )
		return IOutputStream::eWrappingSuccess;

	LOG_DEBUG( "Wrap on stream " << streamIndex << " (" << data.getSize() << " bytes for frame " << _frameCount.at( streamIndex ) << ")" )

	AVPacket packet;
	av_init_packet( &packet );
	packet.stream_index = streamIndex;
	packet.data = (uint8_t*)data.getData();
	packet.size = data.getSize();

	_formatContext.writeFrame( packet );

	// free packet.side_data, set packet.data to NULL and packet.size to 0
	av_free_packet( &packet );

	const double currentStreamDuration = _outputStreams.at( streamIndex )->getStreamDuration();
	if( currentStreamDuration < _previousProcessedStreamDuration )
	{
		// if the current stream is strictly shorter than the previous, wait for more data
		return IOutputStream::eWrappingWaitingForData;
	}

	_previousProcessedStreamDuration = currentStreamDuration;
	_frameCount.at( streamIndex )++;

	return IOutputStream::eWrappingSuccess;
}

bool OutputFile::endWrap( )
{
	LOG_DEBUG( "End wrap of OutputFile" )

	_formatContext.writeTrailer();
	_formatContext.closeRessource();
	return true;
}

void OutputFile::addMetadata( const PropertyVector& data )
{
	for( PropertyVector::const_iterator it = data.begin(); it != data.end(); ++it )
	{
		addMetadata( it->first, it->second );
	}
}

void OutputFile::addMetadata( const std::string& key, const std::string& value )
{
	_formatContext.addMetaData( key, value );
}

void OutputFile::setupWrapping( const ProfileLoader::Profile& profile )
{
	// check the given profile
	const bool isValid = ProfileLoader::checkFormatProfile( profile );
	if( ! isValid )
	{
		const std::string msg( "Invalid format profile to setup wrapping." );
		LOG_ERROR( msg )
		throw std::runtime_error( msg );
	}

	if( ! profile.empty() )
	{
		LOG_INFO( "Setup wrapping with:\n" << profile )
	}

	// check if output format indicated is valid with the filename extension
	if( ! matchFormat( profile.find( constants::avProfileFormat )->second, getFilename() ) )
	{
		throw std::runtime_error( "Invalid format according to the file extension." );
	}
	// set output format
	_formatContext.setOutputFormat( getFilename(), profile.find( constants::avProfileFormat )->second );

	// set common wrapping options
	setupWrappingOptions( profile );
}

void OutputFile::setupWrappingOptions( const ProfileLoader::Profile& profile )
{
	// set format options
	for( ProfileLoader::Profile::const_iterator it = profile.begin(); it != profile.end(); ++it )
	{
		if( (*it).first == constants::avProfileIdentificator ||
			(*it).first == constants::avProfileIdentificatorHuman ||
			(*it).first == constants::avProfileType ||
			(*it).first == constants::avProfileFormat )
			continue;

		try
		{
			Option& formatOption = _formatContext.getOption( (*it).first );
			formatOption.setString( (*it).second );
		}
		catch( std::exception& e )
		{
			LOG_INFO( "OutputFile - option " << (*it).first <<  " will be saved to be called when beginWrap" )
			_profile[ (*it).first ] = (*it).second;
		}
	}
}

void OutputFile::setupRemainingWrappingOptions()
{
	// set format options
	for( ProfileLoader::Profile::const_iterator it = _profile.begin(); it != _profile.end(); ++it )
	{
		if( (*it).first == constants::avProfileIdentificator ||
			(*it).first == constants::avProfileIdentificatorHuman ||
			(*it).first == constants::avProfileType ||
			(*it).first == constants::avProfileFormat )
			continue;

		try
		{
			Option& formatOption = _formatContext.getOption( (*it).first );
			formatOption.setString( (*it).second );
		}
		catch( std::exception& e )
		{
			LOG_WARN( "OutputFile - can't set option " << (*it).first <<  " to " << (*it).second << ": " << e.what() )
		}
	}
}

}
