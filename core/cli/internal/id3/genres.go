package id3

// v1Genres is the ID3v1 genre table — Eric Kemp's original 0-79 plus the
// Winamp extension to 191 — which TCON's numeric forms index into ("17" and
// "(17)" both mean Rock). ffprobe resolves them the same way, and so does the
// firmware (core/codecs/pvmp3/id3_meta.c); all three lists have to agree or a
// track's genre differs between the index and the device.
var v1Genres = [...]string{
	"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
	"Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
	"Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
	"Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
	"Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
	"Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
	"AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
	"Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
	"Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
	"Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
	"Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
	"Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
	"Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
	"Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion",
	"Bebob", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
	"Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock",
	"Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour",
	"Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
	"Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club",
	"Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul",
	"Freestyle", "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House",
	"Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror",
	"Indie", "BritPop", "Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta",
	"Heavy Metal", "Black Metal", "Crossover", "Contemporary Christian",
	"Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop",
	"Synthpop", "Abstract", "Art Rock", "Baroque", "Bhangra", "Big Beat",
	"Breakbeat", "Chillout", "Downtempo", "Dub", "EBM", "Eclectic", "Electro",
	"Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM",
	"Illbient", "Industro-Goth", "Jam Band", "Krautrock", "Leftfield",
	"Lounge", "Math Rock", "New Romantic", "Nu-Breakz", "Post-Punk",
	"Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock",
	"World Music", "Neoclassical", "Audiobook", "Audio Theatre",
	"Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep",
	"Garage Rock", "Psybient",
}
